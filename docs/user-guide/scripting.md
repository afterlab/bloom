# Headless scripting

`bloom-cli` is Bloom's Qt-free v1 host client. It uses the same typed operations, project-session
ownership, render compiler, output analysis, approval, and publication chain as the desktop UI.

```text
bloom-cli new <file>
bloom-cli open <file>
bloom-cli run <script.json>
bloom-cli render [--project <file>] (--frame N | --range A-B) \
  --preset PngRgba8SrgbV1|FlatExrRgba32fLinRec709SceneV1 --out <dir> [--replace] \
  [--require-device]
bloom-cli export --project <file> (--all | --job <id>...) [--replace] [--save]
bloom-cli help [<name>] [--json]
```

`bloom-cli help` lists every command, operation, MCP tool and query kind with a one-line summary;
`bloom-cli help <name>` shows one in full: arguments with their type, unit, range, default and
accepted values, side effects, the result, errors with what to do, and examples for MCP, Python and
`bloom-cli`. A command name finds the command (`bloom-cli help render`), and operations accept their
Python spelling (`bloom-cli help bloom.ops.layer.add_solid`). `--json` prints the entry as JSON, and
`bloom-cli <command> --help` is `bloom-cli help <command>`. An unknown name prints the closest
entries and exits with status 1. The MCP `help` tool and `bloom.help()` print the same text.

Times in operation arguments are an integer frame count at the composition's frame rate, or an
exact `[numerator, denominator]` time in seconds. Ranges with an `end` are end-exclusive.

`run` accepts a JSON array of `{ "op": "<stable-id>", "args": { ... } }` transactions. Each
transaction is validated against the operation registry and receives an expected current revision.
Arguments the registry marks contextual -- `composition`, `time`, `layer`, `selection`, `layers` --
may be omitted; the session fills them exactly as the Python and MCP clients do, so a headless step that
names no composition edits the project's first one.
Malformed arguments, unknown IDs, stale revisions, and command rejection produce a typed diagnostic
and a non-zero exit status. An operation that succeeds but reports an issue -- for example a file
an import skipped while importing the rest -- prints `warning[bloom-cli.operation-warning]` and
keeps the zero exit status; Python and MCP results carry the same issue as a diagnostic with code
`bloom.scripting.operation-warning` and `severity` `warning`. `bloom.asset.import` also accepts a
folder, which contributes the media files below it, exactly as a folder dropped on Assets does,
and the Import options (`asSequence`, `firstFrame`/`lastFrame`, `skipMissingFrames`,
`sequenceFrameRate`, `stillDuration`; see [Interpret Footage](interpret-footage.md));
scripted imports never show the import progress dialog. The run starts from a new in-memory session; use `new` or the facade's
Save As operation when a durable project is required.

Times follow one contract in `run` scripts, Python and MCP: an integer is a whole frame of the
composition the operation edits, at its frame rate, and `[numerator, denominator]` is an exact time
in seconds. `{"op": "bloom.composition.set-duration", "args": {"duration": 48}}` makes a 24 fps
composition two seconds long. `bloom.parameter.set-source` reads `value` in the parameter's own
kind -- a whole number or a member name such as `"screen"` for an enumeration, `true`/`false` for a
Boolean -- and refuses a value out of range with what the parameter accepts. A refused step names
the operation's index in its transaction and the reason; see
[Using Bloom from an agent](mcp.md#edits-and-diagnostics).

`bloom.layer.align-layers` and `bloom.layer.distribute-layers` work in `run` scripts as well. A
headless session has no selection, so name the `layers`:

```json
[{"op": "bloom.layer.align-layers",
  "args": {"edge": "horizontal-centre", "target": "composition", "layers": [12, 15]}}]
```

They measure the layers with the CPU reference evaluation before the step runs; see
[Scripting Bloom with Python](python.md#align-and-distribute).

`render` selects the first composition when no project is supplied. Output keeps alpha; the composition background colour is display-only and is not rendered. A frame writes `frame.png` or
`frame.exr`; a range writes zero-padded `frame.XXXX` sequence files. The command prints the output
preservation report and only exits successfully after publication completes. A file that already
exists is replaced only with `--replace`; without it the command fails and leaves the file as it
was.

`render` also prints `device gpu-frames=N cpu-reference-frames=M`: how many frames the GPU evaluated
and how many took the CPU reference path. Without a GPU device, Bloom renders on the CPU reference
path and prints `warning[bloom-cli.render.cpu-reference]`. With `--require-device`, the command
fails with `error[bloom-cli.render.device-required]` and a non-zero exit status instead: when no
device is available it fails before evaluating anything, and a frame the CPU evaluated anyway is
refused before it is written. `--require-device` covers the PNG and OpenEXR presets.

Example scripts live in [`examples/scripting`](../../examples/scripting/).

Python scripts and the interactive interpreter are described in [Scripting Bloom with Python](python.md).
For stdio clients, see [Using Bloom from an agent (MCP)](mcp.md).

## Layer and node edits from the UI

Every layer and node edit of the Timeline, Viewer and Node editor menus and drops has an operation,
built on the same command as the UI action, so it validates, labels and undoes the same way:

- `bloom.layer.delete` and `bloom.layer.duplicate` take `layers` (layer ids, not node ids) and
  default to the selected layers. Delete removes each layer's Layer Output node, as Delete in the
  Timeline does; duplicate copies it in place directly below the original and outputs one `layer`
  per copy.
- `bloom.layer.add-from-assets` is an asset drop on the Timeline or Viewer
  ([ADR 0030](../decisions/0030-layer-drops.md)): `assets` (image, image sequence, video or audio)
  and `compositions` (nested-composition layers) become layers in one step, first item uppermost,
  starting at `time` (default the playhead), with `placement` `"top"`, `"bottom"` or `"before"` a
  layer, and `sequence` for end-to-end layers. It does not import: run `bloom.asset.import` first.
- `bloom.node.add-asset-source` and `bloom.node.add-composition-source` are drops on the Node
  canvas: an unwired source node. `bloom.node.add-feeding` and `bloom.node.add-fed-by` add a node
  connected to an existing input or output, as dragging a wire to empty canvas does (a node fed
  into an input that is already connected is inserted into that link, what fed the input feeding
  the new node), and
  `bloom.node.insert-reroute` inserts a Reroute into the link that feeds an input.
- Every Merge entry is a layer
  ([Layer Graph Model](../architecture/layer-graph-model.md#merge)). Every layer edit that takes
  `layer` also takes `merge` and `slot` instead; an entry without a layer gets its own entry layer
  in the same undo step. `bloom.merge.set-entry-parameter` (`merge`, `slot`, `role`, `value`,
  optional `time`) sets an entry's position, anchor, scale, rotation, opacity, blend mode or Time
  Remap by role, keyed when `time` is given; `bloom.merge.ensure-entry-layer` returns or creates an
  entry's layer; `bloom.merge.insert-layer-node` splices a Layer node that adopts the entry's
  values; `bloom.merge.add-adjustment-layer` (`merge`, optional `above` slot) is the Timeline's
  Add > Adjustment Layer. `query kind=layers` lists every entry in `entries`.

```json
[{"op": "bloom.merge.add-adjustment-layer", "args": {"merge": 1}},
 {"op": "bloom.merge.set-entry-parameter",
  "args": {"merge": 1, "slot": 2, "role": "opacity", "value": 0.5}},
 {"op": "bloom.layer.set-solo", "args": {"merge": 1, "slot": 2, "solo": true}}]
```

Project Settings > Color is not a document edit, so it is not an operation: MCP's
`project.set-color-settings` tool and Python's `Host.set_color_settings(config, working_space=None)`
choose the OCIO configuration (`"neutral"`, `"aces"` or `"current"`) and working color space
exactly as the dialog does, and return the resulting `config`, the stored working-space id
(`workingSpace` / `working_space`) and its label (`workingSpaceLabel` / `working_space_label`, for
example `lin_rec709_scene` reads "Linear Rec.709 (sRGB)"). Like the dialog, the change is not undoable, marks the project dirty
and changes how every composition renders. In the desktop application's Script editor,
`Host.set_color_settings` answers `unavailable`: use Project Settings > Color there (agents use
the MCP tool, which the window applies).

`query kind=operations` (MCP) and `Host.schemas()` (Python) carry each operation's `description`:
its purpose, side effects, outputs, errors with remedies, undo step and an MCP example.

## Export queue

The export queue ([ADR 0033](../decisions/0033-export-queue.md)) is scriptable in two halves.

**Project-truth operations** run in transactions like any other edit, with the Export panel's
validation and undo: `bloom.export.add-job` (returns `exportJob` and one `exportOutput` per output),
`bloom.export.remove-jobs`, `bloom.export.remove-finished-jobs`, `bloom.export.duplicate-job`,
`bloom.export.move-jobs`, `bloom.export.set-job-name`, `set-job-range`, `set-job-enabled`,
`set-job-label-color`, `bloom.export.add-output`, `bloom.export.remove-output`,
`bloom.export.set-output-settings`, `set-output-destination`, `set-output-enabled`,
`set-output-import` and `bloom.export.apply-preset`. An output travels as a JSON
object: `{"preset", "settings": {"format", "codecProfile", ...}, "destination": {"portability",
"path", "overwrite"}, "enabled", "importIntoAssets"}`, each field optional. An output that names a
built-in `preset` and gives no `settings` takes that preset's settings, as `apply-preset` does
(`{"preset": "builtin:prores-422hq-mov", "destination": {"path": "Exports/{name}.mov"}}`); a preset
that is not built in needs `settings`, and a refused output says why. Without `outputs`, `add-job`
adds one H.264 MP4 output at `Exports/{name}.mp4`.

**Session calls** act on runtime state only (never undoable, never saved): `export.start`,
`export.start-all`, `export.pause`, `export.resume`, `export.stop`, `export.stop-all`,
`export.retry`, `export.confirm-replace` (`job`, `output`), `export.status` (optionally
`sinceRevision`), `export.events` (`afterSequence`), `export.log` (`job`, `afterSequence`),
`export.wait` (a `job` or the whole queue, `timeoutMs`; headless only), `export.presets` and
`export.script` (`job`, `format` `"json"` or `"python"`). In a `run` script, `export.start` without
`job` starts the job the script last added:

```json
[{"op": "bloom.export.add-job", "args": {"range": "custom", "first": 0, "last": 47}},
 {"op": "export.start", "args": {}},
 {"op": "export.wait", "args": {"timeoutMs": 600000}}]
```

Nothing replaces a file silently: an unattended script sets the output's overwrite policy to
`replace` or calls `export.confirm-replace`. `bloom-cli export` renders a saved project's queue
headlessly, prints the verification and import results as JSON and exits non-zero unless every
selected job succeeds; a `--job` id the project does not have is refused before anything renders.
`--replace` confirms existing destinations and `--save` writes asset imports back to the project.
Its scratch files go in a new temporary folder only your user can open, removed when the command
ends. Export runs on Linux, macOS and Windows.

`bloom-cli render`, the Python render call and MCP render keep their request shapes but run as
transient, unsaved export jobs on the same runner as the queue, so every rendered frame passes the
same preservation policy, publication and verification; their range files are named
`<base>.NNNN.<ext>` as before. An output's `useCachedFrames` setting (default `true`) lets an export
reuse frames the composite cache already rendered; set it to `false` to evaluate every frame again.
