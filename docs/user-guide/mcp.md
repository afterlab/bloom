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
a browser origin, too many clients). Identical refusals within a second are counted on one line,
and repeated compile failures of one composition update its line with a count ("×3"). An open
agent session is named on the status line, and an event stream's opening and closing are listed.
Up to 512 lines are kept; "Earlier activity was trimmed." marks a gap.

### Limits

Up to 8 clients at once. When a ninth connects, the least recently used client that has been idle
for 5 minutes with nothing in flight is disconnected; otherwise the new client is refused. A client
idle for 60 minutes is ended and reconnects on its next request. At most 16 connections and 4
requests executing at once. The in-progress limit protects the main thread, so batch many edits
into one `transact` instead of sending them in parallel.

Every request limit is listed in the table below and advertised by `initialize` under
`capabilities.experimental.bloom.limits`, by the name in the first column. A request over one is
refused before anything runs, and the refusal names the limit, its value and the actual count: a
JSON-RPC error with the limit's own code and `error.data` `{code, limit, value, actual, unit}`, or for
the commit bounds a tool result whose `diagnostic` carries `limit` `{name, value, actual, unit}`.
Refusals of the request's size or JSON shape suggest the columnar key form
(`bloom.animation.set-key-columns`) and `{"fromFile": ...}` data files.

A refusal for load (the four marked "busy" below) also says when to try again: `retryAfterMs` in
`error.data` or in the tool's `diagnostic`, the recent commit time (a moving average of how long the
last edits took from their turn to their result, 250 ms before the first) times the work queued
ahead of the request plus one, between 100 ms and 30 seconds. Over HTTP, `Retry-After` carries the
same in whole seconds, rounded up.

| Limit (`limits.<name>`) | Value | Refused with |
| --- | --- | --- |
| `requestBytes`: one request (HTTP body or stdio line) | 1 MiB | `-32003` (HTTP 413) |
| `responseBytes`: one response | 4 MiB | `-32006`; an applied `transact` answers compact instead |
| `jsonValues`: JSON values in one request (array items and member values) | 65,536 | `-32004` |
| `jsonDepth`: levels below the root | 32 | `-32005` |
| `jsonMemoryBytes`: memory one parse or response may take | 32 MiB | `-32007` |
| `dataFilesPerCall`: `fromFile` data files in one call | 16 | `-32009` |
| `dataFileBytes`: one data file | 16 MiB | `-32010` |
| `dataFileValues`: JSON values in one data file | 2,000,000 | `-32011` |
| `dataFileDepth`: levels in one data file | 16 | `-32012` |
| `inFlightPerClient`: requests one client has executing or waiting (1 on stdio) | 4 | `-32008` (HTTP 503 with `Retry-After`), busy |
| `pendingMessages`: requests waiting for a worker, all clients | 16 | `-32000` (HTTP 503 with `Retry-After`), busy |
| `commitQueue`: edits waiting to apply, MCP and the Script editor together (desktop only) | 16 | `bloom.mcp.commit-queue-full`, busy |
| `commitDeadlineMs`: how long an edit may wait to apply (desktop only) | 30,000 ms | `bloom.mcp.deadline-exceeded`, `bloom.mcp.gesture-open` |
| `commitWork`: work in one `transact` (desktop only) | 1,000,000 units | `bloom.mcp.commit-too-large` |
| `transactOutputs`: created ids a `transact` lists in `outputs` | 10,000 | never refused: answered in the summary form |
| `asyncJobs`: asynchronous `transact` jobs queued or running, all clients (desktop only) | 16 | `bloom.mcp.jobs-full`, busy |
| `projectJsonValues`: JSON values the project's `document.json` holds | 6,000,000 | `bloom.mcp.project-too-large` |
| `projectDocumentBytes`: bytes of the project's `document.json` | 224 MiB | `bloom.mcp.project-too-large` |
| `projectContainerEntries`: entries in one list of the project (a curve's keyframes, for example) | 999,000 | `bloom.mcp.project-too-large` |
| `projectStringBytes`: one text value in the project | 64 MiB less 64 KiB | `bloom.mcp.project-too-large` |
| `retainedEvents`: events kept for `events` and every subscription (the one buffer subscribers share) | 1,024 | never refused: a cursor further behind is told `resetRequired` (reason `overflow`) |
| `eventsPerAnswer`: events one `events` answer lists (its `limit`) | 1,024 | a larger `limit` is an invalid argument; past it, `truncated` and continue from `sequence` |
| `eventIntervalMinMs`, `eventIntervalMaxMs`: an `events.subscribe` interval | 50 to 60,000 ms | an invalid argument outside them |
| `eventCompositions`: compositions one notification or summary lists | 64 | never refused: the rest are counted in `compositionsOmitted` |
| `eventJobs`: asynchronous `transact` jobs a subscription follows | 80 | never refused: the oldest finished job is left out |
| `eventStreamsPerClient`: open `GET` streams of one session | 1 | never refused: a newer `GET` ends the older stream |
| `subtitleCues`: cues in one cue list | 50,000 | `bloom.mcp.limit.subtitle-cues` |
| `subtitleCueTextBytes`: UTF-8 bytes of one cue's text | 4,096 | `bloom.mcp.limit.subtitle-cue-text-bytes` |
| `subtitleFileBytes`: one subtitle file, or text pasted as one | 16 MiB | `bloom.mcp.limit.subtitle-file-bytes` |
| `subtitleStyles`: named styles in one cue list | 256 | `bloom.mcp.limit.subtitle-styles` |
| `subtitleActiveCues`: cues that show at one time | 16 | an edit or load that leaves more is refused as an invalid list, naming the time; the `subtitles` query warns on a supplied list that holds more |

`initialize` also reports `requestWorkers`, `clients`, `connections`, `renderFrames`,
`retainedEvents`, `eventOutputs` (the ids an `events` record keeps), `eventsOutputsPerAnswer`,
`retainedJobs` (finished asynchronous jobs that stay listed: 64), `idempotencyKeys` (keys remembered
at once: 1,024; the oldest goes first), `idempotencyKeyBytes` (128), `idempotencyWindowMs` (how long
a key is remembered: 600,000) and `idempotencyAnswerBytes` (the largest answer a key keeps whole:
64 KiB; a larger one is kept in its compact form). For [event subscriptions](#being-told-what-changed-event-subscriptions)
it reports `eventsPerAnswer` (1,024), `eventIntervalMinMs` (50), `eventIntervalMaxMs` (60,000),
`eventCompositions` (64), `eventJobs` (80), `eventStreamsPerClient` (1) and `eventKeepAliveMs`
(15,000); they are in the table below. Nothing that was refused ran, so send it again once the
condition clears.

### Edits, undo and project changes

A `transact` call is one atomic transaction and one undo step, labelled with its `label` or
"Agent edit". Bloom builds and checks it off the main thread, so a large edit does not freeze the
window, and then lands it between your own edits: never while you are in the middle of a drag, a
scrub or a text edit (it waits until you finish, up to 30 seconds), and a revision conflict
leaves the project unchanged. If you edit the project while an agent's edit is being built, yours
lands first and the agent's edit answers with a revision conflict, as if it had come second. One
`transact` carries at most 1,000,000 units of work (one per operation, or one per key, keyframe or
edit an operation lists; initialize reports it as `capabilities.experimental.bloom.commitWorkLimit`);
a larger one is refused with `bloom.mcp.commit-too-large`, so split it into several. A queued edit
that is cancelled, times out or outlives its project is never applied, and the result names why
with a stable code: `bloom.mcp.cancelled`, `bloom.mcp.deadline-exceeded`, `bloom.mcp.gesture-open`,
`bloom.mcp.project-replaced`, `bloom.mcp.commit-queue-full`. The Script editor and MCP share one
queue of 16 edits. The [limits](#limits) table lists every bound an edit can meet.

Clients stay connected when you open or create a project. While a project is opening, calls answer
`bloom.mcp.project-unavailable`; an `events` cursor from the old project reports `resetRequired`, and
an event subscription says so in its next notification.

#### Long edits: asynchronous transact, progress and cancellation

`transact` with `"async": true` answers at once with a job: `{succeeded, async, job:
"transact-<n>", state, phase, queuedAhead, baseRevision, ...}`. The job runs the edit exactly as a
synchronous `transact` would, one job at a time in the order they were sent; nothing else waits for
it. Follow it with:

- **`jobs.wait`** `{job, timeoutMs?}` waits until the job finishes (or `timeoutMs`, default 30 s,
  passes and the answer says `timedOut`) and returns it with `result`: the job's `transact` answer,
  the same one a synchronous call would have given. With `_meta.progressToken` the answer is an
  event stream of `notifications/progress` first. Cancelling the wait leaves the job running.
- **`jobs.list`** lists it after the Jobs panel's rows, with the string id `transact-<n>`, kind
  `transact`, `state` (`queued`, `running`, `succeeded`, `failed`, `cancelled`), `phase` (`queued`,
  `building` its operations, `waiting` behind other edits, `preparing` them, `prepared` and waiting
  for Bloom to publish it, `publishing`, `checking` compositions, `done`), `completed` of `total`, a
  `message` such as "Applying operation 120 of 400", its `revision` once applied and its `code`
  when it did not apply. The newest 64 finished jobs stay listed.
- **`jobs.cancel`** `{job: "transact-<n>"}` stops a queued job at once and a running one before its
  edit starts publishing; nothing is applied either way. An edit that has started publishing
  finishes as one atomic undo step, and `jobs.wait` then shows it `succeeded`.

A job belongs to the project it was sent for: if that project is replaced before the job applies,
the job fails with `bloom.mcp.project-replaced` and never touches the new project. At most 16 jobs
are queued or running at once (limit `asyncJobs`). Asynchronous `transact` needs the desktop
application; `bloom-mcp` answers `bloom.mcp.not-live`.

A synchronous `transact` sent with `_meta.progressToken` reports the same progress as an event
stream before its answer.

#### Retrying safely: idempotency keys

Give `transact` an `idempotencyKey` (1 to 128 printable ASCII characters, such as a UUID) when a
request might be sent twice, for example after a timeout. For 10 minutes the same key with the same
arguments answers what the first request answered, with `idempotentReplay: true`, instead of
applying the edit again; if the first is still running, the second waits for it. With `async: true`
the repeat names the same job (and carries its `result` once it finished). Only an edit that reached
the project is remembered, applied or refused (a stale revision, for example): one that was busy,
cancelled or outlived its project is forgotten, so sending it again runs it. The same key with
different arguments is refused with `bloom.mcp.idempotency-conflict` and nothing is applied. Keys
are shared by every client and kept by both hosts.

#### Being told what changed: event subscriptions

Instead of polling `events`, an agent can be told. `events.subscribe` subscribes the calling MCP
session: `{kinds?, compositions?, intervalMs?, afterSequence?}`, where `kinds` names the event
kinds above plus `job` (the steps of asynchronous `transact` jobs), `compositions` lists the
compositions to hear about (a project-wide change always comes through), and `intervalMs` (50 to
60,000, default 250) is the least time between two notifications. It answers `{subscription,
created, method, sequence, intervalMs, kinds, compositions}`; calling it again changes the filter
and interval in place, and `events.unsubscribe` ends it. Ending the MCP session ends it too.

Bloom then sends `notifications/bloom/events`. The first change after a quiet spell is sent at once,
and everything after it within the interval is coalesced into the next one, so a burst of a
thousand edits is a handful of notifications. Each says what changed since the previous one:

```json
{"jsonrpc":"2.0","method":"notifications/bloom/events","params":{
  "subscription":1,"sequence":212,"fromSequence":181,"toSequence":212,
  "resetRequired":false,"count":16,"fromRevision":91,"toRevision":106,
  "kinds":{"revision":16},"origins":{"agent":15,"user":1},
  "compositions":[{"composition":1,"count":15,"fromRevision":91,"toRevision":106}],
  "compositionsOmitted":0,"project":{"count":1,"fromRevision":97,"toRevision":97},
  "jobs":[{"job":"transact-4","mine":true,"state":"running","phase":"preparing",
           "completed":120,"total":400,"message":"Applying operation 120 of 400",
           "finished":false}]}}
```

`count`, `kinds` and `origins` count the selected events; `compositions` gives each composition's
count and revision range (at most 64; the rest are counted in `compositionsOmitted`), and `project`
the changes that name no composition. `jobs` lists each followed job that moved since the
previous notification, with its newest step (`mine` is true for the session's own). Read the events
themselves with `events` from the previous notification's `sequence`. `events` with `response`
`summary` answers the same object for one call.

Over Streamable HTTP, open the stream with `GET /mcp`, `Accept: text/event-stream` and the
session's `Mcp-Session-Id`. Each notification is a server-sent event whose `id` is its `sequence`;
reconnect with `Last-Event-ID` to continue after it, or without it to continue after the last
notification Bloom sent. A session has one stream: a newer `GET` ends the older one. A quiet stream
sends an SSE comment every 15 seconds, and the stream ends when the subscription ends, the session
ends or the server stops. While its stream is open and reading, a client counts as active, so it is
not ended for idling. The stream holds one of the 16 connections, and the MCP Server editor
lists when it opened and closed. On stdio (`bloom-mcp`) the notifications arrive on stdout between
answers, as JSON-RPC notifications.

Bloom keeps no backlog for a subscriber: each notification is built when it is due from the events
`events` retains (`limits.retainedEvents`, 1,024), which every subscriber shares. A subscriber that
falls further behind than that (a stream that stopped reading, or more changes in one interval than
are retained) gets one notification with `resetRequired: true`, `reason: "overflow"` and `count`
0, and continues from the newest event. When the project is replaced (opened, created or closed,
from the File menu or an agent) the next notification says `resetRequired` with
`reason: "project-replaced"`, and the subscription goes on with the new project. Either way, read
again whatever you cache.

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
| `project.save` | none | `bloom.mcp.no-path` when the project was never saved; `bloom.mcp.read-only` for a read-only project; `bloom.mcp.save-failed` when the file could not be written. |
| `project.save-as` | `path` (absolute, `.bloom`), optional `replace` | `bloom.mcp.destination-exists` unless `replace` is true; `bloom.mcp.path-invalid` for a relative path or a missing folder; `bloom.mcp.save-failed` when the file could not be written. |
| `project.open` | `path`, optional `discardChanges`, `stopJobs` | Replaces the open project. |
| `project.new` | optional `discardChanges`, `stopJobs` | An untitled project. |
| `project.close` | optional `discardChanges`, `stopJobs` | The window shows an empty project. |
| `project.save-copy` | `path` (absolute, `.bloom`), optional `replace` | File > Save a Copy: a byte-for-byte copy of a read-only (newer or preserved) project; the open project stays where it is. An editable project answers `bloom.mcp.invalid-argument` (use `project.save-as`); `bloom.mcp.save-failed` when the copy could not be written. |
| `project.cancel-open` | none | File > Cancel Open: abandons the open in progress, yours or an agent's. `bloom.mcp.invalid-argument` when nothing is opening. |
| `editor.reveal` | `composition`, optional `time` | Shows the composition in the Viewer and moves the playhead. |
| `selection.set` | `composition`, optional `layers`, `nodes`, `mode` (`set`, `add`, `remove`, `clear`, or the Select menus' `all-layers`, `all-nodes`, `invert-layers`, `invert-nodes`, `linked-upstream`, `linked-downstream`) | Contextual `layer`, `layers` and `selection` arguments then default to it. Invert works on the active composition's selection; the linked modes extend from `nodes`, or from the selected nodes, through links and driver bindings. |
| `composition.activate` | `composition` | The Viewer, Timeline and contextual `composition` follow it. |
| `transport` | `action` (`seek`, `play`, `stop`, `step-forward`, `step-backward`, `go-to-start`, `go-to-end`, `ram-preview`), `time` for `seek`, optional `composition` | `time` counts in the given composition, or the active one. `ram-preview` caches the work area and then plays it, as the RAM Preview button and Numpad 0 do; `stop` also ends a RAM preview that is still caching. |
| `viewer.set` | any of `display`, `view`, `exposure` (stops), `gamma` (1.0 is neutral), `channel` (`rgba`, `rgb`, `red`, `green`, `blue`, `alpha`), `zoom` (`"fit"`, `"actual-size"` or a factor, 1.0 is 100%), `resolution` (`auto`, `full`, `half`, `quarter`), `look` (true or false), `background` (`solid`, `gray`, `black`, `white`), `transparencyGrid` (true or false), `regionOfInterest` (`{x, y, width, height}` in composition pixels, or `null` to clear it), `level` (group layer ids from the top, outermost first; `[]` is the top level), `points` (a shape layer of that level whose points are edited, or `null`) | Display settings only: they never reach a render or the project. `level` and `points` are the Viewer's drill level, session state like a double-click. |
| `export.reveal` | optional `job` | Brings the Export editor into view and, with `job`, selects it, as the Jobs panel's **Show in Export** does. `bloom.mcp.unknown-target` for a job the queue does not hold; `bloom.mcp.failed` when the panel could not be shown. |
| `jobs.list` | none | The Jobs panel's rows: `{id, name, phase, state, completed, total, canCancel, cancellationRequested, exportJob}`, then the asynchronous `transact` jobs (string id `transact-<n>`, `kind: "transact"`; see [Long edits](#long-edits-asynchronous-transact-progress-and-cancellation)). An export run's row names its `exportJob`. |
| `jobs.cancel` | `job` | The Jobs panel's **Cancel**, or an asynchronous `transact` job's (`"transact-<n>"`). `bloom.mcp.unknown-target` for a job it does not list (`bloom.mcp.unknown-job` for a transact job); `bloom.mcp.invalid-argument` for a job it cannot cancel (an export run: use `export.stop`) or one that has finished. |
| `jobs.wait` | `job`, optional `timeoutMs` | Waits for an asynchronous `transact` job and returns it with its `result`; streams progress with a progress token. |
| `preferences.get` | none | Every preference below with its current value, as `{key: value}`. |
| `preferences.set` | `preferences`: `{key: value, ...}` (1 to 64) | Edit > Preferences' **OK**: every entry is checked first, then all are saved and applied together. An unknown key or a value out of range answers `bloom.mcp.invalid-argument` naming the key and changes nothing; `bloom.mcp.busy` while a dialog is open. |
| `dialog.answer` | `button` | Presses one button of the modal dialog that blocks the window (see **Dialogs** below). Off unless the artist switches on Edit > Preferences > General > **Let agents answer open dialogs**: until then `bloom.mcp.not-allowed`. `bloom.mcp.invalid-argument` when no dialog is open or no visible, enabled button has that text, listing the buttons. |
| `script.run` | `source`, optional `timeoutSeconds` (1 to 600, default 60) | Runs Python in the open project through the Script editor (see **Python in the running session** below). Off unless the artist switches on **Let agents run Python scripts**: until then `bloom.mcp.not-allowed`. |
| `profiler.snapshot` | none | One reading of the [Profiler panel](profiler.md): memory, GPU, CPU, workload, pressure and helper processes. With the panel closed it asks for a fresh GPU heap query that the next snapshot carries. |
| `cache.purge` | `cache` (`preview` or `media`) | Edit > Purge: derived caches only; the project, its revision and undo are untouched. `bloom.mcp.busy` while a purge runs. |
| `asset.reveal` | `asset` | Reveal in File Manager. `bloom.mcp.invalid-argument` for an asset without a file. |
| `composition.presets` | none | The Composition Settings presets: the built-in sizes, then your saved presets with `frameRate`, `duration`, `startTimecode`, `dropFrame` and `background`. Apply one with `bloom.composition.set-settings`, whose `pixelArt` argument (Boolean) turns the composition's [pixel-art mode](pixel-art.md) on or off. |
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

A `project.save`, `project.save-as` or `project.save-copy` that did not write the file answers
`bloom.mcp.save-failed`, and so does a `project.save` with `projectFile`. Its message is the
sentence the status bar shows ("Save failed while encoding document.json: ...", "Save a Copy failed
while checking the destination file: ...") and its `diagnostic.detail` names the step and the cause
as stable tokens, for example
`{"stage": "publication.staged-save.build.document-encode", "cause": "value-count-exceeded"}`,
`{"stage": "publication.preflight", "cause": "target-leaf-not-regular"}` or, for a copy,
`{"stage": "copy.staged-copy.stage-write", "cause": "stage-write-failed"}`. A copy's stages start
with `copy.`: `copy.admission`, `copy.preflight`, `copy.registration`, `copy.staging`,
`copy.staged-copy.<step>` (writing, finishing, sizing, reading back and verifying the temporary
file), `copy.guard` and `copy.publish` (replacing the destination). The project and the file on
disk are unchanged, so the call can be retried once the cause is fixed. A project too large for
one file never gets this far: the edit that would have made it so is refused at the edit with
`bloom.mcp.project-too-large` (see **Project size** under `transact`). Both servers answer it the
same way; `bloom-mcp` on stdio copies the file itself and names `copy.staged-copy.stage-write` or
`copy.publish` with the system's reason.

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
  A save an agent started never opens a dialog, even when it fails: the agent gets
  `bloom.mcp.save-failed` and the window shows the same sentence in its status bar.
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
  `audioPlayback`, `loopPlayback`, `crashRecovery` (off by default), `automaticUpdateCheck` (on by default), `ramHeadroomBytes` and `vramHeadroomBytes` (0 to 256 GiB), `exportWorkerMemoryCapBytes` (0 for no
  cap, to 1 TiB) and `exportWorkerThreads` (0 for automatic, to 64),
  `mediaDiskCache`, `mediaDiskCacheDirectory` (absolute, or empty for the default),
  `mediaDiskHeadroomBytes` (the space to keep free on the cache drive: 0 for the default, to
  4096 GiB; applies at once), `mediaLocalCopies` (`automatic`, `always` or
  `never`), `mediaLocalCopyFolders` (one line per folder rule, `keep:<absolute folder>` or
  `skip:<absolute folder>`; empty for none), `timelineTimeFormat` (`frames` or `timecode`),
  `timelineSnapping`, `timelineKeyframesVisible`, `timelineGraphEditor`, `nodeLinkStyle` (`spline`,
  `straight`, `angled`), `nodeSnap`, `nodeGridSize` (pixels), `nodeShowZoomControls`,
  `nodeShowMinimap`, `viewerResolution` (`auto`, `full`,
  `half`, `quarter`), `viewerBackground` (`solid`, `gray`, `black`, `white`),
  `viewerTransparencyGrid`, `viewerSafeAreas`, `viewerCenterCross`, `viewerThirds`, `viewerRulers`,
  `viewerPixelGrid`, `importSequenceFrameRate` (`"24"`, `"24000/1001"`, or empty for one image per
  composition frame), `importStillDuration` (seconds, `"5"` or `"1001/240"`, or empty for the
  composition's length), `sounds`, `soundVolume` (0 to 100), `eventSounds`, `interactionSounds`,
  `renderNotifications` and `agentActivityFeedback` (Sounds and notifications for agent activity,
  on by default).
- **`jobs.list`** and **`jobs.cancel`** read and cancel what the Jobs panel shows: imports, previews,
  thumbnails, purges and export runs.

Selection, the active composition, the playhead, the Viewer's settings, preferences, cache purges,
jobs and composition presets are session or per-user state: they never mark the project as changed
and never enter the undo history. An edit that leaves a
composition unable to render also appears in the MCP Server editor's activity list as a red line
("left 'Main' unable to render: ...") and in `transact`'s `compileFailures`.

`render`, `export` and `capture` run as transient jobs of the export queue, with the same checks,
verification and Viewer priority as the Export editor; they are never saved. Their output keeps
alpha unless `background` is `composition` (see below). Nothing replaces an
existing file silently: for `render` and `export`, `"overwrite":"ask"` (the default) and `"refuse"`
both fail with `bloom.mcp.export-unavailable` when the destination exists, because nothing can
answer the question over MCP; pass `"overwrite":"replace"`. Queue jobs that ask wait for
`export.confirm-replace`. Right after Bloom starts, the encoders are still being checked: an `export`
of a movie preset in that moment waits for the check (up to 30 seconds, and it ends when the request
is cancelled) and then answers with the real availability, so a client never has to retry. A check
that takes longer still fails with `bloom.mcp.export-unavailable` and "Checking encoders…".

#### Quiet agent work

Bloom keeps an agent's work quiet. A `render`, `export` or `capture` plays no sound and posts no
notification of its own. A refused `transact` plays no refused cue and flashes no status message:
it is answered to the agent and listed in the MCP Server activity. An edit that breaks a
composition's compile warns once per composition per 10 seconds, and its repeats are counted on
one activity line ("×3"). After 10 quiet seconds Bloom tells what the agent did once, for example
"Agent: 37 renders, 1 failed". The summary is a sound, plus an operating-system notification when
Bloom is not the active application. When the artist is in Bloom, there is no summary at all.
Edit > Preferences > Sounds > **Sounds and notifications for agent activity**
(`agentActivityFeedback`) is on by default; off makes agent activity silent. See
[sound feedback](../ux/sound.md#agent-activity).

#### Agent sessions

A burst of many edits (building a shot, a batch of fixes) can run as one **agent session**:

| Tool | Arguments | Result |
| --- | --- | --- |
| `session.begin` | optional `label` (up to 256 bytes), `quiet`, `deferViewer`, `groupUndo` (each true by default), `idleTimeoutSeconds` (30 to 3600, default 600) | `{succeeded, session}` |
| `session.end` | optional `session` (the id `session.begin` answered; this client's session when absent) | `{succeeded, session}` with what it did |

`session` is `{id, label, quiet, deferViewer, groupUndo, idleTimeoutSeconds, open, closed,
commits, undoSteps, artistEdits, seconds}`. `closed` is `null` while it is open, then `ended`,
`client-disconnected`, `idle-timeout`, `project-replaced` or `server-stopped`.

While a session is open:

- **`groupUndo`**: the session's commits are one undo step, "Agent session “label”". One Edit >
  Undo takes the whole session back. The step keeps only the project as it was before the session
  and as it is now, never one copy per commit, so a session of thousands of edits holds no more
  memory than two edits do. A step that would exceed the undo history's memory limit is closed and
  the session continues in a new one. A `history` undo inside the session takes back the session's
  step so far, and the next commit starts a new step.
- **The artist's own edit** is never folded into the agent's step and is never refused because the
  window was behind. It closes the session's current step, becomes a step of its own, and the
  agent's next commit starts a new session step after it. `session.end` reports `artistEdits` and
  `undoSteps` (1 unless the artist split the session or `groupUndo` was false).
- **The panels** (Timeline, Nodes, Properties and the rest) catch up with the agent's commits about
  once a second instead of rebuilding for every commit. Outside a session they rebuild at most once
  per display frame for a burst of agent commits. `query`, `transact` and `events` always see
  the newest revision, so the agent keeps passing the `revision` of its last read as before.
- **`deferViewer`**: the Viewer does not re-render for each commit. It refreshes when the session
  ends, when the agent calls `editor.reveal`, `composition.activate`, `transport` or `viewer.set`,
  and when the artist presses a key or a mouse button, scrolls or touches the window. The artist's
  input always catches the window up before it lands. `capture` and `render` are unaffected: they
  render the newest revision.
- **`quiet`**: the session's renders and warnings are held and told once when it ends, as "Agent
  session finished" with the label, under the rules in **Quiet agent work** above.

One session is open at a time, in the whole application. A second `session.begin` answers
`bloom.mcp.busy` naming the open one. A session closes on `session.end`, when its client
disconnects, when its client sends nothing for `idleTimeoutSeconds` while it has no request in
flight, when a project is opened, created or closed, and when the server stops. However it closes,
the undo step is closed, the window catches up and the summary is told once. A `session.end` after
it closed answers `bloom.mcp.invalid-argument` naming why. The MCP Server editor's status line shows
an open session, and its activity lists when each session began and ended. Sessions exist only in
the in-app server: `bloom-mcp` answers `bloom.mcp.not-live`.

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
again. A task tool (`render`, `export`, `capture`, `export.wait`, `jobs.wait`) or a `transact` that
carries `_meta.progressToken` is answered as `text/event-stream`: `notifications/progress` events,
then the result (a call that finishes before it has progress to report answers as plain JSON). `GET` (with `Accept: text/event-stream` and the `Mcp-Session-Id`) opens the session's stream of server-initiated notifications once it called `events.subscribe` (see [Being told what changed](#being-told-what-changed-event-subscriptions)); without a subscription it is 405, and without that `Accept` 406. Requests from a browser `Origin`, a
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
headless status, tool and resource support, every request limit (`experimental.bloom.limits`, see
[Limits](#limits)) and short `instructions` that point an agent at `help` and the getting-started
guide.

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
| `query` | `kind`: project, compositions, layers, nodes, node-types, parameters, parameter, parameter-value, keyframes, assets, subtitles (see [Subtitles](#subtitles)), operations, context or enums (the interpolation, ease and Easy Ease vocabularies). layers, nodes, parameters, parameter, parameter-value and keyframes also need `composition`. `compositions` records carry size, `frameRate`, `fps`, `pixelAspect`, `duration` and `start` as `{frames, seconds}`, `dropFrame`, `workArea` (end exclusive, or null), `backgroundColor` (display-only), `workingColorSpace`, `safeAreas`, `layerCount` and `folder`. `layers` lists a composition's layers topmost first: `index`, `slot`, `merge`, `layerOutputNode`, `sourceNode`, `sourceType`, enabled, solo, locked, label colour, parent, `start` and exclusive `end`, `timing` (start time, speed, before and after modes, extent), masks, track matte, and the parameter ids of the layer's transform (`parameters`) and its source (`sourceParameters`). `nodes` records carry `name`, `category`, `parameters`, `position`, `width`, `collapsed`, `muted`, `group`, `inputs` (port, kind, required and the connected `source`) and `outputs` (port, kind and `targets`); Merge nodes add their `stack`; the result adds `links` and `groups`. `node-types` lists every type `bloom.node.add` accepts; with `nodeType` it returns one type with each parameter's metadata. A node's `category` (in `nodes` and `node-types`) is its group in the Add menu ([UI grammar](../ux/ui-grammar.md)): one of `sources`, `compositing`, `channels`, `keying-mattes`, `transform`, `distort`, `blur-sharpen`, `light-shadow`, `stylize`, `color-correction`, `color-management`, `time`, `text-paths`, `instances-particles`, `tracking-analysis`, `fields`, `values`, `math`, `convert`, `string`, `logic`, `time-values`, `color-values`, `vector`, `utilities` or `output`. Earlier builds reported `filters`, `grade`, `color` and `layers`, and reported the value nodes as `values`, `math` or `utilities` only. `parameter` with `parameter` returns the record and `metadata`: `valueKind`, `animatable`, `default`, `unit`, `displayScale`, `minimum`, `maximum`, `softMinimum`, `softMaximum`, `choices` (`{value, label}`; `bloom.parameter.set-source` takes either the value or the label, matched case-insensitively, and refuses anything else naming the valid choices) and `owner`; write values in stored units (the units of `minimum`, `maximum` and `default`), while `unit` and `displayScale` are how Properties shows them (an opacity stored as 0.5 shows as 50 %). `keyframes` with `parameter` (an animated one) or `curve` returns the keys sorted by time: `keyframe`, `component` (x, y, z, red, green, blue, alpha, or null), `time` as `{frames, seconds}`, `value`, `interpolation` (hold, linear, ease-in-out) and the ease handles, paged like parameters (`pageSize` 1–64, `nextCursor`). `parameters` without paging returns full records; with `pageSize` (1–64) and optional `cursor` it returns bounded summaries and `nextCursor`; constant values are explicitly marked `valueOmitted`. `parameter-value` with `parameter` ID and `expectedRevision` reads UTF-8 string chunks using optional byte `offset` and `limit` (4–131072). `assets` records carry `kind` as a name (image, sequence, audio, video, lut), `path`, size, `frames`, digest, `folder` and `tags`, and the result lists the Assets `folders` (`id`, `name`, `parent`); an image, sequence or video also carries its `interpretation` (the [Interpret Footage](interpret-footage.md) members `bloom.asset.set-interpretation` takes, plus the resolved `playbackFrameRate` and `filePixelAspect`). `layers` records also carry `shy`, `nameFollowsSource` and `sourceName`, and (every Merge entry is a layer, see [Merge entries and adjustment layers](#merge-entries-and-adjustment-layers)) `layerKind` (`node` for a Layer node, `entry` for a Merge entry's own layer, whose `layerOutputNode` is null and whose `merge` and `slot` own it), `entryLayer`, `pivot` (`content` or `frame`), `entries` (`[{merge, slot}]` of every entry it is the layer of), `shared`, `adjustment` (the Adjustment node serving its entry, or null) and `values` (`{role: value}`, null for an animated or driven control); the `layers` result adds `entries`, every Merge entry (output Merge first, each top to bottom) whether or not it has a layer yet: `merge`, `slot`, `index`, `outputMerge`, `resolvedLayer` (the entry's layer id, or null), `layerKind` (`node`, `entry` or `none`), `entryLayer`, `layerNode`, `sourceNode`, `sourceType`, `name`, `enabled`, `solo`, `locked`, `shy`, `pivot`, `adjustment`, `shared`, `nestedMerge` (the Merge it shows, for a nested Merge), `parameters` and `values` (an entry without a layer reports the defaults it renders with). A Merge node's `stack` rows (`nodes`) carry the same entry fields after `slot`, `layer` and `index`. A `parameter` of an entry layer reports `owner` `{layer, merge, slot, role}` and the Layer node's metadata. An operations record carries a `description`, every argument's `kind`, `required`, `contextual`, `context`, `summary`, `unit`, `minimum`, `maximum`, `default` and `choices`, plus a pasteable `example` call. `context` returns the editor context contextual arguments fall back to: project, composition, selected nodes and layers, time (seconds as `[n, d]`), the Viewer's display and view, the Viewer's drill `level` (entered group layers, outermost first) and `points` (the shape in point editing, or null), read-only state and `live` (false on stdio, which reports the session defaults, an empty level). |
| `transact` | `expectedRevision`, an `operations` array of `{op,args}`, and optional `label`, `response` (`full`, the default, or `summary`) and `compileCheck` (default `true`). Returns one atomic command result, created IDs, structured diagnostics and `compileFailures` and `compileWarnings` (see [Edits and diagnostics](#edits-and-diagnostics)). `outputs` lists at most 10,000 created ids (`transactOutputs`): past that the answer is in the summary form, `outputSummary` `{count, byName, digest}`, with `outputsSummarized` naming the limit. At most 256 diagnostics are listed (the rest are counted in `diagnosticsOmitted`). A transaction that applied is always answered: one whose full answer would exceed the response bound comes back compact, with `responseCompacted: true`. Arguments the descriptor marks `contextual` may be omitted and are filled from the session (the live editor context in the desktop application). Coded failures carry `structuredContent.diagnostic` `{code, message}` with a `bloom.mcp.*` code. |
| `render` | `composition`, `frame` or inclusive `first`/`last` (each defaults to the composition's own first or last frame, so neither renders the whole composition; an image sequence is limited to 10,000 frames, a movie is not), `preset`, `destination`, optional `overwrite` (`ask`, the default, `replace` or `refuse`). Returns `publishedFrames`, `preservationReport` and `files` with SHA-256 digests. Output keeps alpha unless optional `background` is `composition` (`transparent`, the default, or `composition`: every frame over the composition's Background Color, opaque). With `loudnessLufs` the audio is normalized to that EBU R128 integrated loudness under the true-peak ceiling, and the result carries `loudness` (measured and delivered level and true peak, the gain, and the limiter's deepest reduction; a level that could not be measured, as in silence, is `null`). Optional `subtitles` `{sidecar: [srt, vtt, ttml], nodes}` also writes each Subtitles node's cues as sidecar files beside the frames; see `export`. |
| `export` | Render arguments plus optional `profile`, `audio`, `sampleRate`, `loudnessLufs` (-40 to -5) with `truePeakDbtp` (-12 to 0, default -1; the limit holds in the delivered file: its sound is decoded back and measured, and a file still over the limit is not published but rendered again with the limiter lower by the overshoot, at most three more times, which the result's `log` says, or `preservationReport` for a ProRes, DNxHR or WAV export the stdio server runs itself), `encoder` (`automatic`, the default, `software` or `hardware`; on macOS `automatic` makes ProRes with the system's encoder and fails if it cannot, and `software` uses Bloom's FFmpeg encoder), `workerMemoryCapBytes` and `workerThreads` (see [Export worker limits](#export-worker-limits)). Optional `subtitles` `{sidecar: [srt, vtt, ttml], embed, nodes}` delivers the subtitles: sidecar files `<name>.<language>[.<n>].<format>` beside the output, and with `embed: true` tracks inside an MP4, MOV or WebM movie (asking to embed in MXF, an image sequence or WAV is refused and names the sidecar formats); `nodes` names the Subtitles nodes, otherwise every node with Export as track on. The result's `subtitles` lists each delivered track (`node`, `language`, `title`, `format` `srt`, `vtt`, `ttml` or `embedded`, `path`, `cues`, and `bytes` and `sha256` for a sidecar), and `files` lists each sidecar with its digest; see [Export](exporting-video.md#subtitles). Uses the existing MEDIA-4 composition runner for ProRes preview, DNxHR and PCM output. Output keeps alpha unless `background` is `composition`. A movie's or WAV's result lists `streams` read back from the file; MP4 carries AAC-LC, WebM Opus at 48 kHz, MOV and MXF PCM. |
| `events` | `sinceRevision`, optionally `afterSequence`, `limit` (1 to 1,024, the default), `kinds`, `compositions` and `response` (`events`, the default, or `summary`). Returns revision-inclusive events, a sequence cursor and `resetRequired` when retained history cannot cover the cursor. Each command event carries `outputCount`; it lists its created ids only when it made at most 1,024 (otherwise `outputsOmitted: true`). One answer lists at most `limit` events and 8,192 ids: when it stops early, `truncated` is `true` and `sequence` is the last event listed; pass it as `afterSequence` to continue. `kinds` and `compositions` select events (see [Being told what changed](#being-told-what-changed-event-subscriptions)); `response` `summary` answers the same ranges and counts a subscription's notification carries instead of the events. |
| `events.subscribe`, `events.unsubscribe` | Optional `kinds`, `compositions`, `intervalMs` (50 to 60,000, default 250) and `afterSequence`. Subscribes this MCP session to coalesced change notifications, or ends the subscription; see [Being told what changed](#being-told-what-changed-event-subscriptions). |
| `history` | `action` (`undo` or `redo`) and `expectedRevision`. One step of the project's history, refused with a stale-revision result when the revision moved. In the desktop application it is queued like a `transact`, and it undoes the most recent step of any origin, including the artist's. |
| `capture` | Optional `composition` and `frame` (defaults: the context composition at the context time; on stdio the session's composition at frame 0). Renders one PNG at composition resolution through the still export route, with the Viewer's display and view in the desktop application (not its exposure, gamma, channel, region of interest or preview resolution), and returns it as MCP `image` content. Output keeps alpha unless `background` is `composition`. A PNG larger than about 2.9 MiB is returned as a `resource_link` to the file with the notice `bloom.mcp.capture-too-large-for-inline`. With `background` `composition` the capture is composited over the composition's Background Color. Captures go to an `mcp-captures` folder in Bloom's per-user cache, which keeps the newest 16. |
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
- `bloom.layer.set-filter` (`composition`, `layer`, `filter`; or `merge` and `slot` for an entry)
  sets how the layer's transform samples its picture: 0 Bilinear, 1 Nearest, 2 Trilinear,
  3 Anisotropic or 4 Automatic. It creates the layer's optional `filter` parameter when the layer
  has none; a layer without one is Bilinear. Automatic is Nearest in a pixel-art composition.
- `bloom.layer.set-snap` (`composition`, `layer`, `snap`; or `merge` and `slot` for an entry) sets whether
  the layer's final position rounds to whole output pixels: 0 Off, 1 On or 2 Automatic (on in a pixel-art
  composition). It creates the layer's optional `snap` parameter; a layer without one does not snap.
- `bloom.cryptomatte.pick` (`node`, `point` `[x, y]` in composition pixels as `capture` shows them,
  optional `mode` `add` or `remove`, `time` contextual) is the Viewer's Cryptomatte eyedropper: it
  maps the point through the layer that shows the node, reads the object with the most coverage
  there and adds it to (or removes it from) the node's matte list by its manifest name, or its raw
  ID without a manifest. A point outside the layer or over no object answers "No object under the
  point". To choose mattes by name, set the node's `mattes` text with
  `bloom.node.set-parameter-value`.
- `bloom.layer.add-from-assets` (and the other ways to add a nested composition as a layer) finds the
  composition's layer Merge even when effect nodes sit between it and Output (a finishing chain of film grain,
  chromatic aberration and the like), adds the layer to that Merge and leaves the chain as it is. It is refused
  only when no layer Merge is upstream of Output, and the refusal says what it looked for.
- `bloom.export.add-job` takes `outputs` as objects, for example `"outputs": [{"preset": "builtin:h264-mov",
  "destination": {"path": "/exports/a.mov"}}, {"preset": "builtin:prores-422-mov", "destination": {"path":
  "/exports/b.mov"}}]`, or as their JSON text; `bloom.export.add-output` takes its `output` the same way. A refused
  output is named, as `outputs[1]`, with the reason.
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
  `blendMode` (a value or a label such as `"screen"`), `filter` (0 to 4, which creates the entry
  layer's filter), `snap` (0 to 2, which creates its snap) or `timeRemap` (only while Time Remap is on)
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

### Subtitles

A Subtitles layer draws a list of timed cues ([Subtitles](../architecture/subtitles.md) is the
specification). `bloom.layer.add-subtitles` adds one from `cues`, a subtitle `file` (SRT, WebVTT,
TTML or ASS) or pasted `text`, and outputs `subtitlesNode` and one `cue` per cue. The list is edited
with `bloom.subtitles.set-cues`, `insert-cue`, `edit-cue`, `remove-cues`, `shift-cues` and
`scale-cues`, each one undo step that keeps the ids of the cues it leaves. `load` replaces the
list with one read from a file or text, `reload` reads a linked file again, `unlink` stops the
list following its file, and `write-file` writes the list as SRT, WebVTT or TTML without changing
the project.

```json
{"name": "transact", "arguments": {"expectedRevision": 2, "operations": [
  {"op": "bloom.layer.add-subtitles", "args": {"composition": 1, "cues": [
    {"start": "00:00:01.000", "end": "00:00:03.500", "text": "Hello."},
    {"start": [7, 2], "end": 120, "text": "<i>Second</i> cue", "speaker": "Ana"}]}}]}}
```

A cue is `{id?, start, end, text, speaker?, style?, placement?}`, where `placement` is `{anchor,
positionX, positionY, lineOffset, width, textAlign}`. Cue times take the usual frames and
`[n, d]` seconds, and also a timestamp `"HH:MM:SS.mmm"` or `"HH:MM:SS,mmm"`, exact in
milliseconds. The end is exclusive. A large list travels as `{"fromFile": "/abs/cues.json"}` or
through `load`. A list past one of the `subtitle*` limits is refused before anything applies, with
the limit's own code and `diagnostic.limit`.

A linked list (`link: true`) follows its file. In the app Bloom checks the file about once a
second and reloads a change as one undo step, with a notice in the status bar. Every edit except
`load`, `reload` and `unlink` is refused on a linked list, and so is an edit of a list another node
supplies through the Cues input. `bloom-cli render` and an export never reload: a file that changed,
is missing or cannot be read is warned as `bloom.subtitles.linked-file-changed`, and the render uses
the cues the project holds.

`query` `kind` `subtitles` with `composition` lists its Subtitles nodes. With `node` it gives the
list's count, language, provenance, linked-file state (`none`, `in-sync`, `changed`, `missing` or
`unreadable`) and styles, then a page of cues (`pageSize` 1–64, `cursor`): each with `id`, `index`,
`start`, `end` and `duration` as `{frames, seconds}`, a display timestamp, `text`, `plainText`,
`speaker`, `style`, `placement` and warnings. `at` keeps the cues showing at one time; `from` and
`to` keep those overlapping `[from, to)`. `format` `srt` or `vtt` answers the whole list as that
file's text. A change event names the changed cue ids in its outputs (`cue`, `removedCue`).

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
lists all three. A Number List parameter takes `{"values": [n, ...]}`, at most 65536 finite
numbers (`docs/architecture/number-list.md`), and is read back in the same shape;
`bloom.node.write-to-number-list` stores what a node's list output gives at a time in a Number List
node, which is how a computed list is read, and `bloom.node.bake-to-keyframes` reads a number
output at every frame of the composition and stores it as keyframes on a new Number node
(`docs/user-guide/audio-analysis.md`, "Baking to keyframes"). A value out of range is refused with what the parameter
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

**Project size.** A project Bloom lets you build always saves. A `transact` that would make the
project larger than one project file can save and reopen is refused at the edit with
`bloom.mcp.project-too-large`, and nothing is applied. Its `diagnostic.limit` names the limit the
edit would pass (`projectJsonValues`, `projectDocumentBytes`, `projectContainerEntries` or
`projectStringBytes`, advertised under `limits`), its value and what the project would hold
(`actual`); when the edit would pass the file format's own limit, `actual` is that limit and the
message says "at least". The message is the sentence the window shows, for example "The edit was not
applied: the project would hold 6,008,881 JSON values, more than the 6,000,000 one project file can
hold. Remove keyframes, layers or compositions you no longer need, or move part of the work into
another project." The project as it stands still saves, and an edit that does not grow the project
past the limit (a rename, removing keys) still applies. Bloom measures every edit with the same
encoder a save uses, so the limit a `transact` meets is the limit the save would have met. In the
app the measure runs while the transaction is prepared, on the request's own thread, and counts
only what the edit changed, so it never holds up the window; a transaction prepared again because
the artist edited meanwhile is measured again.

**Compile failures.** After a successful edit, every composition whose evaluation inputs it
changed is compiled (never rendered); the edit's own commit names them, and a change outside any
one composition (an asset, for example) compiles every composition. `compileFailures` lists those
that cannot render now:
`[{composition, code, detail, subject: {composition, node, parameter}}]`, where `detail` is one
plain sentence and `node`/`parameter` are `null` when the failure has no narrower subject. Each one
is also recorded as a `composition.compile-failed` event. An empty list means every touched
composition compiles. `compileChecked` says whether the check ran: `transact` with
`"compileCheck": false` skips it, for bulk key writes that cannot break a composition, and then
answers `compileChecked: false` with both lists `null`. At most 256 records of each list are
returned; the rest are counted in `compileWarningsOmitted` or `compileFailuresOmitted`.

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

**Events.** Besides command events (`kind` `revision` when the project changed, by an edit, an
undo or a redo; `history` when the undo history changed, which follows every applied step; or
`rejected`), `events` reports `{kind, revision, message, composition?, subject?, path?,
projectName?, sequence}` records for `composition.compile-failed`, `project.saved`,
`project.opened`, `project.created`, `project.closed` and `project.copy-saved`. A command event
names the compositions it changed when the edit could name them; one that cannot (a project-wide
change such as the project's name or an asset) names none and matches every `compositions`
filter.

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
- `kind:"outputs"` with `node`, optional `composition`, `time` and `expectedRevision` evaluates the
  node at that time and lists every output with its socket `kind` and `value`: numbers, vectors
  (`[x, y]`), colours, text, paths and number lists as their parameters are written, and a space as
  `{name, dimensions, matrix, affine, range, parent}` (`matrix` is four rows of four: units to
  composition pixels). Nothing has to be connected to the node. Picture, sound and field outputs
  read `value:null` with a `note`; `diagnostics` lists what the node reported.
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
- `kind:"scopes"` measures a frame (or `frame` to `lastFrame`, at most 1000 frames) exactly as the
  Scopes panel does and returns the count tables, a summary computed from them, or a PNG of the
  scope. `composition` and `frame` (a whole frame number) or `time` choose the frame; `scopes` lists
  any of `waveform`, `parade`, `vectorscope`, `histogram` and `chromaticity` (default all). `signal`
  is `display` (through the configuration's default display and view unless `display` and `view`
  name another pair) or `working` (with `axisMaximum` 1, 2, 4, 8 or 16). `region` is
  `[x, y, width, height]` in composition pixels, `ignoreTransparent` leaves out pixels whose alpha
  is 0, and `resolution` is `full`, `half` or `quarter`. A read never uses the Viewer's exposure,
  gamma, channel, isolation or region of interest. `result` is `tables` (the default), `summary`
  (per channel min, max, mean, p1, p50 and p99 code values and clipped counts, with the
  vectorscope's mean saturation and hue; for the display signal the clipped counts are the
  frame's `frameCounters`) or `image` (`image` is one scope or `all`, `width` and
  `height` 128 to 2048 pixels, plus `waveformChannels`, `histogramChannels` and
  `histogramScale`; the picture is returned like a `capture`, inline when small and as a resource link
  otherwise, and a range of images is limited to 16 frames). Tables are flat arrays: each has
  `shape`, `order` and `values`, and the histogram and waveform also `under` and `over`;
  `encoding:"sparse"` lists only the non-zero cells as `[index, count]` pairs. The answer reports
  `settings` as measured, the `luma` coefficients, the colorimetric `primaries`, and per frame the
  `route` (`gpu` or `cpu`) with a `routeNote` saying why the CPU reference counted it,
  `fromCache` (the application's preview cache supplied the tables), `measuredArea`, `counted`
  and the counters of pixels not finite, without chromaticity or outside a plot. The display
  signal is the frame as it is stored for display: 10 bits a channel, or 8 bits for a frame with
  partly transparent pixels. A stored code is never outside 0 to 1, so the tables' `under` and
  `over` are always 0 for the display signal; what the display encoding left outside 0 to 1 is in
  each display frame's `frameCounters` (`under` and `over`, four numbers each in the order red,
  green, blue, luma, with `null` for luma when it was not counted, and `notFinite`), which count
  the whole frame whatever the region and `ignoreTransparent`, and `storedLayout` (`"10:10:10:2"`
  or `"8:8:8:8"`). A working frame has neither. An answer too
  large for one response is refused with the ways to make it smaller. In the desktop application a
  frame whose tables the preview cache already holds, counted by the frame's own preparation under
  exactly the read's settings, is returned without measuring; a display read of a frame the cache
  holds as the same picture (the same display and view, no exposure or gamma) counts the cached
  frame's stored cells and renders nothing. The same read is `bloom.scopes.read` and `bloom.scopes.image` in Python.
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
`replace`, with the same units and refusals as `set-keys`, and the same one-pass merge: the cost of
a key write grows with the keys written and the curve they go into, not with every key the
composition already holds. A column, or any other operation
argument, can be `{"fromFile": "/absolute/path/data.json"}`: the server reads that JSON file (a
regular `.json` file of at most 16 MiB and 2 million values, at most 16 per call), so the numbers
never pass through the conversation. Add `"response": "summary"` to `transact` and the answer
carries `outputSummary` (`count`, `byName` and a SHA-256 `digest` of the full id list) instead of
listing every created id. Python and `bloom-cli` have the same operation.

**Many parameters at once.** `bloom.animation.set-key-tracks` keys many parameters in one
operation and one undo step: `tracks` is a list of `{parameter, times, values, interpolations?,
replace?}`, each exactly the arguments of `set-key-columns` for one parameter (`parameter` is an id
or a `{"layer", "role"}` reference), and each merged with the same one-pass merge. Every track is
checked: a track with uneven columns or a parameter that already has a track is refused before
anything runs, and a track the project refuses when it applies (an unknown parameter, a value of the
wrong width, two keys at one time) refuses the whole operation, so nothing is applied; the message
names the track. Its work is every track's keys, within the one-commit bound of 1,000,000. The whole
list can come from one data file:

```json
{"name": "transact", "arguments": {"expectedRevision": 7, "response": "summary",
  "operations": [{"op": "bloom.animation.set-key-tracks",
    "args": {"composition": 1, "tracks": {"fromFile": "/absolute/path/tracks.json"}}}]}}
```

with `tracks.json` holding
`[{"parameter": {"layer": "Title", "role": "opacity"}, "times": [0, 12], "values": [0, 1]},
{"parameter": 42, "times": [0, 24], "values": [[0, 0], [100, 50]], "interpolations": ["hold"]}]`.
Python takes the same list of dictionaries (`bloom.ops.animation.set_key_tracks([...])`) and
`bloom-cli` scripts the same JSON.

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
`project.save`, `project.close` and the `export.*` queue tools take `projectFile`, the absolute
path of a project file other than the open one. The call runs in a headless session of that file,
with the same operations; the session stays open between calls (edits, undo and unsaved changes
persist), `project.save` with `projectFile` writes it and `project.close` releases it
(`discardChanges:true` to close with unsaved edits; `stopJobs:true` when its export jobs still
run, which are otherwise not stopped silently).

The file has an export queue of its own, the same queue the Export panel and `bloom-cli export`
run, so a `render` or `export` of it works exactly as one of the open project: every preset
`export.presets` lists, progress while it runs, and a result with its `job` id, `log`, `files`,
`streams` and `preservationReport`. Pass the same `projectFile` to `export.status`,
`export.wait`, `export.stop`, `export.log` and the other `export.*` tools to reach that queue:
`export.stop` with the file and the job id stops its export while the `export` call waits, and
`export.start` runs the file's saved jobs. Job ids belong to their queue, so a call without the
`projectFile` does not find the file's job. A movie exported this way is the same file, byte for
byte, as one exported from the open project or by `bloom-cli export --project <file> --job <id>`.

A file that is open in Bloom, or is the server's own project, is that open project: a call with
its `projectFile` attaches to it (the edit lands in the artist's project and undo history, as
without `projectFile`) instead of opening a second copy. The result then adds
`attachedToOpenProject: true` and `fileDiffers`: whether the project open in Bloom differs from
the file on disk. When it differs (changes in Bloom that are not saved, or a window showing a copy
that is not the file's), `fileDifference` says so and `reload` offers the call that makes them
agree, `project.open` with the path and `discardChanges: true`; `project.save` keeps Bloom's
copy instead. `project.close` with that `projectFile` answers `closed: false`: there is no
separate session to close. A session that already held the file when the artist opened it in
Bloom is closed when it has no unsaved changes, so calls attach; with unsaved changes the call is
refused with `bloom.mcp.preflight-blocked` and what to do (`project.close` it with
`discardChanges:true`, or have the file closed in Bloom), and its `project.save` is refused
because it would overwrite Bloom's file. When Bloom is busy and cannot say within two seconds
whether the file is open there, the call is refused (`bloom.mcp.project-unavailable`, "Bloom is
busy ...") instead of assuming it is free. While a session holds a file, `project.open`,
`project.save-as` and `project.save-copy` onto that path are refused with `bloom.mcp.busy`;
close the session first. At most four files are open this way at once. In the desktop
application their renders use the application's own final-render provider, only for these calls;
the open project keeps rendering through the export queue. A render, export
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
`export.status` lists it in `jobs` with `transient: true`.

A failed `render`, `export` or `capture` answers `bloom.mcp.export-failed` with the export's own
failure code as `diagnostic.detail` and a message that names the frame and why. A frame that takes
long to render is never failed for being slow: the frame publishes once it is ready, and only the
export's own limits stop it (120 seconds without progress in a writer, 24 hours for one export),
with `export.publication.timeout` and the limit in the message. `export.publication.memory` means
the computer had no memory for writing the file, `export.publication.output-path` names what was
wrong with the destination or Bloom's export scratch folder, and `export.publication.writer` names
the format writer. A frame whose publication is refused memory that other work holds waits for it,
and stops as `export.capacity.stalled` only when nothing released any for 30 seconds. A failure
that can succeed unchanged once other work lets go of memory or of the output file
(`export.capacity.stalled`, a refused memory reservation, a busy output file) carries
`diagnostic.retryAfterMs` (5,000): send the same call again after it. The queue retires the job when the call
returns, but Bloom keeps the newest 32 finished ones: `export.status` lists them in
`recentTransientJobs`, and `export.log` and `export.wait` still read each one by its id. Opening or
creating another project releases them.

Export job and output ids (`export.status`, including its `activeJob`, `export.events`, the `job` of a
failure, `jobs.list`'s `exportJob`) are reported as decimal strings, such as `"4503599627370497"`.
Every id is at most 2^53 - 1, so even a JSON client that parses numbers as doubles holds it exactly,
and `help` says the same. Pass the
string back as `job` or `output` (or in the `jobs` list of `export.start`) to `export.start`,
`export.pause`, `export.resume`, `export.stop`, `export.retry`, `export.confirm-replace`,
`export.log`, `export.wait`, `export.presets`, `export.script` and `export.reveal`. A plain integer
is still accepted where it is exact (a client that sends the literal digits unchanged).

Output keeps alpha by default: a composition's `backgroundColor`
(`bloom.composition.set-background-color`; `query` with `kind` compositions reads it back as
straight `[red, green, blue, alpha]` display values) is the Viewer's display backdrop. Where nothing
covers the frame, a still stays transparent exactly as a sequence or a video format that carries
alpha does, on the GPU route and the CPU reference alike. `render`, `export` and `capture` take an
optional `background`: `transparent` (the default) or `composition`, which composites every frame
over the composition's Background Color in the working space before the output transform, so the
output is opaque; a nested composition never draws its own. The preservation report then ends in
`background=composition (opaque over composition background)`, and a queued output stores the same
choice as `"background": "composition"` in its settings.

The `preservationReport` of a `render` or `export` reads the same whichever route ran it, the
export queue (the desktop application, the queue presets, and every `projectFile` session) or the
scripted render (`bloom-cli render`, Python's `bloom.render`):
`preset=<id>; exact-facets=<n>/<m>; approvable=<true|false>`, where `<id>` names the output
preset (`PngRgba8SrgbV1`, `ProResMovV1`, `H264Mp4V1` ...), `exact-facets` counts the
preservation facets the published output keeps exactly, and `approvable` says it was published
without a failure; the background phrase above is appended when it applies.

A movie's or WAV's `export` result, and its output's `verification` in `export.status`, list
`streams`: `[{kind, codec, profile, width, height, frameRate, channels, sampleRate, bitrate}]` as
read back from the written file (`frameRate` is `[numerator, denominator]`; `bitrate` is in bits
per second). MP4 carries AAC-LC audio at 192 kb/s, WebM Opus at 192 kb/s and always 48 kHz, MOV and
MXF 16-bit PCM (24-bit when a source is deeper) and WAV 16- or 24-bit PCM; each `export.presets`
row states `container`, `videoCodec`, `videoProfile`, `audioCodec`, `audioProfile`,
`audioBitrateKbps`, `audioSampleRate`, `pcmDepth` and an `audio` sentence. The resource
`bloom://guide/exporting` serves the export user guide.

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
values, and JSON depth at most 32. Arrays have no separate item cap. Parsing and writing allocate
what the message needs, at most 32 MiB each, and a request is parsed once. The pending request
queue holds at most 16 messages (see [Limits](#limits) for every bound and its refusal). Duplicate decoded keys, malformed Unicode, non-finite JSON numbers, invalid ID
types and unknown schema members are rejected. A render request is limited to 10,000 frames; event
history retains 1024 records. Clients should advance `afterSequence` and re-query the snapshot when
`resetRequired` is true. This local process uses its launcher's filesystem permissions.

Protocol references: [transports (stdio and Streamable HTTP)](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)
and [tools](https://modelcontextprotocol.io/specification/2025-11-25/server/tools).
