# Image Switch

Status: implemented (#531). Owner: `src/document/image_switch_nodes.cpp`,
`src/runtime/snapshot_compiler_lowering.ipp`, `src/runtime/image_switch_liveness.hpp`.

Two nodes choose one of several inputs by an index: **Image Switch** (`bloom.image-switch`, eight
Image inputs `input1` to `input8`, an Image output) and **Channel Set Switch**
(`bloom.channel-set-switch`, eight Channel Set inputs `channels1` to `channels8`, a Channel Set
output). Bloom has no port that takes either kind, so the two share one rule, one schema
(`bloom.image-switch.index`, `bloom.image-switch.wrap`) and one compiled operation.

## The rule

- **Index** (`index`, Float64, animatable, default 1, with a Scalar socket of the same name so an
  Integer or Scalar value node drives it). Its whole part counts from 1: 2.7 shows input 2. A keyed
  index therefore steps at each whole number.
- The switch **counts** its inputs from the first up to the highest one connected. An unconnected
  input below that shows nothing (a transparent, empty image or an empty channel set).
- An index below 1 or above the count is **clamped** to the first or last counted input, or with
  **Wrap** (`wrap`, Boolean, default off) wrapped modulo the count, both ways: with three inputs, 4
  shows input 1 and 0 shows input 3.
- A switch with no counted input, or an index that is not finite, shows nothing.
- A muted switch shows input 1.
- `document::imageSwitchPosition` is the one implementation of the rule; the compiler, both render
  routes and the value output all call it.

The `selected` output (Integer) publishes the input shown, counted from 1, and 0 when the switch
shows nothing. It is how another node reads the choice: an animated Image Switch of a beauty pass
drives the Channel Set Switch of its render passes, so the passes follow the beauty on every frame
(the render-pass rule of #346).

## Compilation

A **constant** index (a constant parameter, nothing on the socket) is resolved when the graph is
compiled. Render reachability (`consumesEdge` in the compiler and its document twin in
`render_reachability.cpp`) consumes only the edge of the shown input, so the other inputs' nodes are
never compiled, and the switch is an alias of the shown input's operation. The plan holds no switch
and is the plan of the same input wired directly, with the same cache identity.

An **animated or driven** index compiles to one `CompiledImageSwitch` operation listing every counted
input in port order (an unconnected one is an empty Merge) with the index as a
`CompiledScalarParameter` on the switch's clock and the Wrap choice.

A **Channel Set Switch** with a constant index compiles away the same way: its reader (a Shuffle, a
Pass, a Composition Output channel layer, Cryptomatte, depth and motion readers) reads the shown
channel set. With an animated index, each Shuffle or Pass it feeds is lowered once per input
(`lowerThroughChannelSwitches`, at most 64 readers per switch, refused past that) and the readers
are joined by a `CompiledImageSwitch` with the switch's index. Any other reader of an animated
Channel Set Switch is a compile failure naming the reason ("An animated or driven Channel Set
Switch can feed only a Shuffle or a Channel Pass ..."): written channel layers and Cryptomatte read
the channel set itself, not an image of it. Switch their images, or give the switch a constant
index.

## Per-frame liveness

`liveOperations` (`image_switch_liveness.hpp`) walks back from the output and from every bounds
readout's image, through every input of every operation, but through only the chosen input of each
switch. It generalizes the activity-window skip: an operation it does not reach is not evaluated,
decoded, prepared or dispatched for that frame.

- The CPU reference (`CpuCompositionEvaluator`) gives an unread operation a fixed content hash
  ("unread-switch-input"), releases its inputs and moves on; a frame's `evaluatedNodes` and cache
  misses name only the chosen branch.
- GPU scene preparation (`CpuGpuSceneBuilder`) narrows its reachable set with the same function
  after `pruneIdentityCodecInputs`, so the prepared scene holds no command of an unselected branch.
  The static plan check still classifies every operation the plan holds.
- Visible-region demand passes a switch's demand to its chosen input unchanged.

## Identity and the image

A switch is its chosen input on both routes: it publishes that input's image, bounds, vector leaf and
content hash (CPU), and its command, key, window, bounds and vector leaf (GPU). It emits no command
of its own and costs no pixel work. Its cache identity is therefore the chosen input's identity:
returning to a frame already drawn evaluates and dispatches nothing, and a change to an unselected
branch does not invalidate a frame that does not show it. A switch that shows nothing is an empty
image with the identity "switch-shows-nothing".

A text, shape or solid leaf seen through a switch is still a vector leaf of the layer that reads it:
the layer resolves its input through the switch (`shownOperation`) on both routes, so the text is
drawn exactly as when wired directly, including on the integer grid. A raster layer chain (#502)
stops at a switch, as it does at an identity effect.

## Cancellation, budget and retirement

The switch adds no pixel storage and no task. Its frames are cancelled, budgeted and retired with the
operations it shows: an unread branch holds no storage, and a frame past the pixel budget is refused
on the CPU route (with the pixel-storage budget diagnostic) and by GPU preparation.

## GPU coverage

`CompiledImageSwitch` is a Required operation in `gpu_coverage_contract.hpp` (the `Switch` lowering
maps to `operation.CompiledImageSwitch`), with Required fixtures `operation.CompiledImageSwitch`,
`node.bloom.image-switch` (a Text leaf through the switch) and `node.bloom.channel-set-switch` (a
Pass and a Shuffle of a layered EXR joined by the switch), each at indices 1, 2, a clamped and a
wrapped one, at full frame, an ROI and a half-scale proxy. There is no opt-out.

## Tests

- `bloom.runtime.image_switch_render_native`: real documents through the production compiler, the
  CPU reference and the GPU route (512x288, a few frames): the rule table, constant pruning,
  animated frames equal to the input wired directly, evaluation counters of the chosen branch only,
  empty and muted inputs, cache identity, Channel Set Switch constant, animated and driven, the
  `selected` output keeping passes in sync with the beauty, the refused written-layer case, budget
  and cancellation, save and reopen, then CPU/GPU parity (2e-6) cold and warm, and the prepared
  scene of an unselected blur holding no command and dispatching what the input wired directly does.
- `bloom.ui.gpu_node_viewer_evidence.image-switch` and `-transformed`, and their `ram-` variants:
  the Viewer GPU-resident with no full-frame readback, and a cold then warm RAM Preview over a keyed
  index.
- `bloom.scripting.image-switch-parity`: MCP, Python and `bloom-cli run`.
- `bloom.runtime.gpu_coverage_contract` and `bloom.runtime.gpu_coverage_native_acceptance`.
