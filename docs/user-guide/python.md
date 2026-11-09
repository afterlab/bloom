# Scripting Bloom with Python

The optional Python client uses the same operation registry, command stack, immutable snapshots,
task scheduler and reference render runner as the native host. Run a trusted local script with
`bloom-cli run script.py`, or start `bloom-cli python` for an interpreter with `bloom` pre-imported.
The embedded interpreter ignores `PYTHONHOME`, `PYTHONPATH`, user site packages and current-directory
imports. It loads the build's Bloom package and the selected interpreter's standard library.

## Building

Install a GIL-enabled CPython interpreter and its development headers. `BLOOM_BUILD_PYTHON` defaults
to ON when the interpreter, extension headers and embedding library are found. Explicit ON requires
all three; explicit OFF disables development-library discovery and the Python clients. MCP pipe
tests may still use a discovered Python interpreter as their test driver. Linux CI installs `python3-dev`
and enables Python explicitly. The current local qualification uses CPython 3.14.7; Linux CI's
system Python may have a different minor version. Rebuild the dependency prefix's nanobind core for
each Python SOABI. This host-interpreter policy is a development intake, not a bundled release
interpreter or a stable-ABI wheel promise. Desktop and headless builds use the same package.

The superbuild provides nanobind 3.0.1 and its separately recorded robin-map 1.4.0 subtree. Normal
application configuration performs no Python package installation or dependency download.
The build-tree package is in `build/<configuration>/python`; an external matching interpreter can
import it when that directory is explicitly placed on its import path. The install destination is
`lib/bloom/python`. `.pyi` files and `py.typed` describe the public package.

## First five minutes

Open the Script editor, or run `bloom-cli python`, and type these lines. Nothing here names a
composition or a time: both come from what you are looking at.

```python
bloom.context                                   # <bloom.context project=... composition=... >
bloom.ops.layer.add_solid("Red", (1, 0, 0, 1))  # a red solid in the current composition
help(bloom.ops.layer.add_solid)                 # arguments, defaults and an example call
```

Key the solid's position X across the first second as one undo step, render a frame, and undo:

```python
position = bloom.data.nodes(bloom.context.composition)[-1].parameters["position"]
with bloom.transactions.group("Slide the solid"):
    bloom.ops.animation.create_for_parameter(position, time=0)
    bloom.ops.animation.set_keyframe_at_time_for_parameter_component(position, 0, 200.0, time=0)
    bloom.ops.animation.set_keyframe_at_time_for_parameter_component(position, 0, 1200.0, time=24)

bloom.render.frame(12, out="frame-12.png")
bloom.transactions.undo()
```

`examples/scripting/first_five_minutes.py` is the same session as a runnable script:
`bloom-cli run examples/scripting/first_five_minutes.py`.

If a call is missing something, the rejection says which argument, what it expected and a call you
can paste:

```
>>> bloom.ops.layer.add_solid()
OperationError: bloom.layer.add-solid: argument 'name' is required.
Example: bloom.ops.layer.add_solid(name="Red", color=(1, 0, 0, 1))
```

## Read data and discover operations

`bloom.help()` prints every operation, MCP tool, query kind and command with a one-line summary,
and `bloom.help("bloom.layer.add-solid")` (or `bloom.help(bloom.ops.layer.add_solid)`) prints one
entry in full: arguments with type, unit, range, default and accepted values, side effects, the
result, errors with what to do, and examples for Python, MCP and `bloom-cli`. `json=True` returns
the entry as a dict. Every `bloom.ops` function's docstring, and `bloom/ops.pyi`, carry the same
entry, so `help(bloom.ops.layer.add_solid)` and an editor's hover show it too. The MCP `help` tool
and `bloom-cli help` print exactly the same text.

```python
import bloom
import inspect

composition = bloom.context.composition
print(composition.id, composition.name)
print(bloom.context.time, bloom.context.selection)
print(bloom.app.version, bloom.app.capabilities)
print(inspect.signature(bloom.ops.layer.add_solid))

for operation_id, operation in bloom.ops.registry().items():
    print(operation_id, operation.schema)
```

Proxies print as what they are: `<Composition 1 'Composition 1'>`, `<Node 12 'Solid'>`, and
`<Node 12 (stale)>` once the object is gone. `repr(bloom.context)` summarizes the project,
composition, selection size and time.

`bloom.data.snapshot()` returns an immutable revisioned projection. Collections such as
`bloom.data.compositions`, `bloom.data.nodes(composition)` and
`bloom.data.parameters(composition)` return stable-ID proxies. `collection.get(id)` resolves by ID;
integer indexing is only a collection position. A removed object raises `StaleObjectError`, carrying
`id` and `revision`. Assigning to a proxy or snapshot does not change the project.

Every registered operation has a callable under `bloom.ops`: hyphens become underscores, so
`bloom.layer.add-solid` is `bloom.ops.layer.add_solid`. Python keywords receive a trailing
underscore (`bloom.asset.import` becomes `bloom.ops.asset.import_`). Every registered ID has a
complete argument schema and a working factory, so everything you can discover you can call.

Calls take positional arguments in schema order, keywords, or both. IDs may be integers or live
proxies; vectors and colors may be tuples or lists; a time may be a whole frame, a `Fraction`, a
float or an exact `(numerator, denominator)` pair. An `int` is a whole frame of the composition the
operation edits, at its frame rate; a `Fraction`, a float and a pair are seconds. At 24 fps,
`set_duration(48)` and `set_duration(Fraction(2))` both make a two-second composition.
`bloom.ops.parameter.set_source` takes `value` in the parameter's own kind: a number, `True` or
`False`, text, a tuple, or for an enumeration a whole number or the member's name (`"Screen"`). A
refused call raises `bloom.OperationError`, whose `diagnostics` carry `operation_index` (the
operation's place in its transaction group) and a message naming the cause.

An edit's result also carries `compile_warnings`: after an edit that changed the project, each
edited composition is compiled once (never rendered), and what still compiles but degrades is listed
as `[{composition, code, detail, subject: {composition, node, parameter}}]`, as MCP `transact` does
in `compileWarnings`. For example a text layer whose Font asset was removed, or whose system font
file is missing or changed, is drawn in DejaVu Sans and reports
`bloom.runtime.compile.font-asset-unavailable`. An empty list means nothing degrades.

```python
bloom.ops.layer.add_solid("Red", (1, 0, 0, 1))
bloom.ops.layer.add_solid(name="Red", color=[1, 0, 0, 1], position=(960, 540))
bloom.ops.composition.set_duration(Fraction(3, 2))
```

`bloom.ops.composition.set_settings` is the Composition Settings dialog as one undoable operation:
pass only what changes. Frame rate and pixel aspect are exact numerator/denominator pairs, `duration`
and `start` (the timecode of frame 0) are times, an empty `workingColorSpace` returns to the project
space, and `anchor` (0 to 8, row by row from top left, default 4 for center) decides where existing
layers sit when the size changes. Layer and key times are never rewritten. Composition presets are
per-user application settings of the desktop app and are not operations.

```python
bloom.ops.composition.set_settings(width=3840, height=2160, anchor=0)
bloom.ops.composition.set_settings(frameRateNumerator=30000, frameRateDenominator=1001,
                                   dropFrame=True, start=Fraction(3600))
```

Arguments that name what you are already looking at default from `bloom.context` when you omit
them: `composition`, `time`, `layer`/`selection` for operations that edit the selection, and
`layers` (every selected layer) for align and distribute. They
stay keyword-only, so an explicit target always reads as one. The schema carries the same
`contextual` flag that the CLI and MCP clients use, so all three apply identical defaults, and
`help()` names the context value each one falls back to. When the session cannot answer — a
headless run has no selection — the rejection says which `bloom.context` value was missing rather
than reporting a bare missing keyword.

`help(bloom.ops.layer.add_solid)` prints the signature, every argument with its kind, whether it is
required, optional or filled from context, and an example call. `dir(bloom.ops)` lists the operation
families and `dir(bloom.ops.layer)` the operations in one. `bloom.ui`, `bloom.addons`, `bloom.props`
and contribution types are reserved stubs whose unsupported access raises `NotImplementedError` with
ADR 0022 and ADR 0012 references.

### Align and distribute

`bloom.ops.layer.align_layers` lines layers up and `bloom.ops.layer.distribute_layers` spaces their
centers evenly, exactly as the toolbar's selection actions do
([ADR 0031](../decisions/0031-contextual-tool-bar.md),
[Layer Alignment](../architecture/layer-alignment.md)):

```python
red, green, blue = 12, 15, 18  # layer ids, or layer proxies from bloom.data.layers()
bloom.ops.layer.align_layers("left", target="composition", layers=(red, green, blue))
bloom.ops.layer.align_layers("vertical-middle", target="key-object", key=red,
                             layers=(green, blue))
bloom.ops.layer.distribute_layers("horizontal", layers=(red, green, blue))
```

`edge` is `left`, `horizontal-centre`, `right`, `top`, `vertical-middle` or `bottom`; `axis` is
`horizontal` or `vertical` and needs three or more layers. `target` is `selection` (the default:
the union of the layers' bounds), `composition` (the frame) or `key-object` (the `key` layer, which
never moves). The operation measures each layer's evaluated visual bounds at `time` with the CPU
reference evaluation of the current project, then moves Position in each layer's parent space; an
animated Position gets a key at that time. Locked layers and layers whose Position is driven are
skipped and reported as warnings (`movedLayer` and `skippedLayer` outputs name them); if no layer
can move, the call raises `OperationError` and nothing changes. The result is one undo step. The
measurement sees the project as it is before the transaction, so put an alignment after the edits
it depends on, in a transaction of its own.

### Merge entries and adjustment layers

Every image fed into a Merge is a layer with the full layer controls
([Layer Graph Model](../architecture/layer-graph-model.md#merge)). An entry's layer is the Layer
node nearest the Merge on its chain, or the entry's own entry layer, or none yet: an entry
without a layer renders exactly as an entry layer at its defaults (position the composition
centre, anchor `(0, 0)`, scale `(1, 1)`, rotation 0, opacity 1, Normal, pivot `frame`).

Every layer operation that takes `layer` also takes `merge` and `slot` instead. When the entry has
no layer yet, its entry layer is created in the same undo step as the edit, and the result also
outputs `layer` and the new `parameter.<role>` ids. `layer` together with `merge` and `slot` is
refused, and a selected layer never fills `layer` when `merge` is given.

```python
entries = bloom.data.merge_entries(bloom.context.composition)
plate = entries[-1]                          # the bottom entry of the output Merge
bloom.ops.layer.set_solo(solo=True, merge=plate["merge"], slot=plate["slot"])
bloom.ops.merge.set_entry_parameter(plate["merge"], plate["slot"], "opacity", 0.5)
bloom.ops.merge.set_entry_parameter(plate["merge"], plate["slot"], "position", (960, 400), time=24)
bloom.ops.merge.set_entry_parameter(plate["merge"], plate["slot"], "blendMode", "screen")
added = bloom.ops.merge.add_adjustment_layer(plate["merge"])           # at the top
bloom.ops.merge.insert_layer_node(plate["merge"], plate["slot"])
```

- `bloom.ops.merge.set_entry_parameter(merge, slot, role, value, time=None)` sets one control of
  the entry's layer by role: `position`, `anchor`, `scale`, `rotation`, `opacity`, `blendMode` or
  `timeRemap` (only while Time Remap is on). Without `time` the parameter becomes the constant;
  with `time` it is keyed there. It works whatever the entry's layer is.
- `bloom.ops.merge.ensure_entry_layer(merge, slot)` returns the entry's layer (`layer` output),
  creating its entry layer when it has none, with its `parameter.<role>` ids for the animation
  operations.
- `bloom.ops.merge.insert_layer_node(merge, slot)` splices a Layer node into the entry. The picture
  does not change: the node adopts the entry layer's values in the same undo step. Outputs `node`,
  `layer` and `edge`.
- `bloom.ops.merge.add_adjustment_layer(merge, above=None)` adds an Adjustment node feeding a new
  entry directly above the slot `above` (default: the top) with an entry layer named
  "Adjustment Layer N". Outputs `node`, `layerSlot` and `layer`. Wire filters between the
  Adjustment node's `image` output and the entry.

`bloom.data.merge_entries(composition)` lists every Merge entry, the output Merge first and each
Merge top to bottom: `merge`, `slot`, `index` (0 is the top), `output_merge`, `layer` (the
entry's layer id or `None`), `layer_kind` (`"node"`, `"entry"` or `"none"`), `entry_layer`,
`layer_node`, `source_node`, `source_type`, `name`, `enabled`, `solo`, `locked`, `shy`, `pivot`
(`"content"` or `"frame"`), `adjustment` (the Adjustment node serving it, or `None`), `shared`
(its Layer node is the layer of more than one entry), `nested_merge` (the Merge it shows, for a
nested Merge), `parameters` (`{role: id}`) and `values` (`{role: value}`: the constant, `None` for
an animated or driven control, or the default an entry without a layer renders with). Layer
records (`bloom.data.layers(composition)`) also carry `entry_layer`, `layer_kind`, `entry_slot`
(an entry layer's slot; its `node_id` is then the Merge), `pivot`, `entries` (`[{merge, slot}]`),
`shared`, `adjustment`, `parameters` and `values`.

## Transactions and undo

```python
with bloom.transactions.group("Build a title"):
    bloom.ops.project.set_name(name="Title study")
    bloom.ops.layer.add_text(composition=composition, name="Title", text="Hello, Bloom!")

bloom.transactions.undo()
bloom.transactions.redo()
```

The group captures an expected revision on entry. `expected_revision=` can supply an explicit
revision token. Operations are collected, validated and committed atomically as one undo entry.
An exception inside the block discards the pending operations. Nested groups are rejected.
Operations inside a group return `None`; the group's `result` becomes available after exit, with
created IDs in `outputs`. Outside a group, an operation returns its immutable command result.
Headless authoring and history calls run on the host's creating thread.

## Building a scene

`bloom.scene` is a small library on top of transactions for scene setup, wiring and timed
animation. You declare layers, parents, mattes, links and keys, then commit the plan:

```python
from bloom import scene

plan = scene.Scene("Title card")
backdrop = plan.solid("Backdrop", (0.04, 0.04, 0.08, 1))
title = plan.text("Title", "Hello", position=(960, 540), size=96)
badge = plan.shape("Badge", "star", position=(200, 200))
title.animate("opacity", [(0, 0.0), (12, 1.0)], ease="ease-in-out")   # frames, layer time
title.animate("position", [(0, (960, 600)), (1.0, (960, 540))], seconds=True)
title.parent_to(backdrop)
badge.set("opacity", 0.5).matte_with(backdrop)
result = plan.commit()         # result.layers == {"Backdrop": 1, "Title": 2, "Badge": 3}
plan.undo()                    # takes back every step the commit made
```

Nothing changes until `commit()`. It sends the plan through `bloom.transactions` in two steps,
because wiring and keys need the ids of what the first step created: one transaction adds every
layer and node, and a second sets values, parents, mattes, links and keys. Each is an ordinary
undo step of the same commands every other edit uses, so `result.steps` says how many undos it
took (`plan.undo()` does them all). Keys go out as columns in one `bloom.ops.animation.set_key_columns`
call per parameter, so a long key list is one operation. `plan.node(name, node_type)` declares a node
of any registered type; a node's `.set(role, value)`, `.animate(role, keys)` and
`.feeds(other, port)` wire it. Layers and nodes are named in the plan; names must be unique
within it. Times are composition frames unless you pass `seconds=True`.

An agent connected over MCP can run this library through `script.run` when the artist has
allowed it; see [Using Bloom from an agent](mcp.md).

## Rendering and tasks

```python
result = bloom.render.frame(12, composition=composition, out="frame-12.png")
print(result.files[0].path, result.files[0].digest)

sequence = bloom.render.range(0, 23, out="frame.png")
again = bloom.render.frame(12, composition=composition, out="frame-12.png", replace=True)
```

The destination directory must already exist. An existing file is never replaced unless the call
passes `replace=True`; without it the render raises `RuntimeError` and leaves the file as it was. Frame labels are zero-based and range endpoints are
inclusive. PNG and scene-linear float OpenEXR use the host's reference compile, analysis, approval,
publication and frame naming paths. `RenderResult` and `RenderFile` are immutable. Digests are
SHA-256 over the published file bytes. These calls release the GIL during native work; no Python
runs in the evaluator or render kernels. Native progress is available through
`bloom.tasks.snapshots()` and native task IDs can be cancelled with `bloom.tasks.cancel(id)`.
A render that is cancelled raises `bloom.CancelledError`; any other failure raises `RuntimeError`.
`RenderResult.gpu_frames` and `RenderResult.cpu_reference_frames` count the frames the GPU evaluated
and the frames that took the CPU reference path, for example on a computer without a GPU device.
Pass `require_device=True` to fail instead of using the CPU: the call raises `RuntimeError` before
evaluating anything when no device is available, and a frame the CPU evaluated anyway is refused
before it is written. This is the same rule as `bloom-cli render --require-device`.
Every `_bloom.Host()` in one interpreter renders through the same GPU device, which is released
once when the interpreter exits.

Long Python work uses a dedicated Python worker and a bounded queue:

```python
def count(task, total):
    for index in range(total):
        task.check_cancelled()
        task.report_progress(index + 1, total, "Counting")
    return total

handle = bloom.tasks.submit(count, 1000)
print(handle.result())
```

The host task scheduler observes progress and cancellation without executing the Python callable
on a native worker. `handle.cancel()` requests cooperative cancellation. The client traces Python bytecode for cancellation; native calls must still cooperate
and return between bounded slices. Complete persistent edits on the authoring thread after
retrieving the result. An in-process native extension that hangs cannot be forcibly cancelled
safely. Shutdown stops queued work and waits for running trusted work to finish.

## Events

```python
with bloom.events.subscribe(lambda event: print(event["kind"], event["revision"])):
    bloom.ops.project.set_name(name="Observed")
    bloom.events.pump()
```

Subscriptions are explicitly closeable. Native callbacks enqueue values; a Python callback runs
only when the thread that subscribed calls `pump()`. The pump is bounded and does not run callbacks
inline on a render or scheduler worker. Native and subscriber queues each retain 1024 events. If
either overflows, the next pump delivers `{"kind": "reset"}` before retained events, regardless of
the subscription's kind filter; re-query the snapshot when that happens. Native session access is
serialized while rendering or saving, and waiting readers release the GIL so another thread can
still observe task progress or request cancellation.

## The Script editor

Choose **Script** in any editor area's panel switcher. Enter a Python statement and press
**Ctrl+Enter** (also the numeric keypad Enter), or click **Run**. `bloom` is pre-imported and variables
persist for the application session. The history dropdown recalls the last 32 inputs. Use
`exec("line one\nline two")` for a block, or run an existing trusted script with Python's file APIs.
Output and tracebacks appear in the panel. Multiple Script areas share the interpreter; closing an
area leaves its work running. Python-disabled builds show an unavailable explanation in this editor.

`bloom.context` follows the live project, composition, selection and time. A transaction enters the
UI command queue and commits on the UI thread, at most one queued transaction per pump slice.
The normal **Edit → Undo** action reverses a whole Python transaction. Expected-revision conflicts
leave the current document intact. Project New/Open invalidates old bindings and proxies and cancels
pending work; the next input binds to the newly installed project. Use the UI Save command to save
this live document.

For example, with a layer selected, animate its position X:

```python
composition = bloom.context.composition
node = bloom.data.nodes(composition).get(bloom.context.selection[0].id)
parameter = node.parameters["position"]
with bloom.transactions.group("Key position"):
    bloom.ops.animation.create_for_parameter(
        composition=composition, parameter=parameter, time=(0, 1))
    bloom.ops.animation.set_keyframe_at_time_for_parameter_component(
        composition=composition, parameter=parameter, component=0,
        time=(1, 2), value=400.0)
```

Use the parameter roles in the node's `parameters` projection; some selected nodes do not own a
position parameter. Component IDs follow the host's semantic enum: X=0, Y=1, Z=2, Red=3, Green=4,
Blue=5, Alpha=6. See `examples/scripting/keyframe.py` for a selection-independent example.

Python runs on a dedicated scripting thread; the UI thread only executes queued host transactions.
**Cancel** stops queued scripts, requests native task cancellation, and interrupts traced Python
work. Foreground GUI execution has a two-second Python-thread CPU budget; put longer computation
or renders in `bloom.tasks.submit`. The footer displays native task progress. Each input is bounded
to 65,536 characters, pending input/transaction queues to 16 entries, and retained output to 262,144
characters. The console runs trusted local code; tracing is a responsiveness measure, not isolation
from malicious Python or a native extension that never returns.
