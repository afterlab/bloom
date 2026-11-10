# Path Processing

Status: built (#344, #530, #509, #637, #598). This document owns the nodes that take a Path and answer
a Path (Transform Path, Trim Path, Offset Path, Boolean Paths, Merge Paths, Round Corners, Path Stroke,
Resample Path and Warp Path) and the two that read a Path from what an artist already drew (Mask Path
and Text Outline). The value they work on is [path-value.md](path-value.md). They live in Text & Paths
and are value nodes, so they appear in a value graph and feed Shape Source's `path` socket like any
other Path producer.

## What kind of node

Each is a value-library node (`ValueUtilityKernel`, value-node-library.md) whose operand is a **Path
operand**: a socket of the Path kind whose unconnected value is the empty path. A Path operand holds a
constant or a driver and never keys, as Shape Source's `path` does; keys belong to the Path node, and a
keyed Path drives the operand through its output.

The computation is a pure function of the operands and does not read the time. It is a *host value
computation*, in the same class as Function Plot: the path every route gets is the same path, and what
draws it is Shape Source's production fill, which is already covered with CPU/GPU parity on every route
(the interactive viewer, still, sequence, video and scripted renders). No node here produces pixels, so
none adds an image lowering to the GPU coverage contract and none needs an exception.

## Limits and failure

| Limit | Value |
| --- | --- |
| Coordinate magnitude (`kMaximumPathCoordinate`) | 1,000,000,000 pixels |
| Flattening tolerance of Offset, Boolean and Merge | 0.001 to 1000 pixels (default 0.25) |
| Anchors per contour, anchors per value, contours | The Path value's own caps |

A non-finite operand, a coordinate beyond the bound, a tolerance outside its range, a result with a
contour over the anchor cap (raise the tolerance), or a path too detailed to flatten fails the
evaluation with a diagnostic and answers the empty path. Nothing is clamped silently except the miter
limit (1 to 100). Work is bounded by the Path value's caps, so one evaluation is bounded work.

Cancellation is observed inside every kernel as well as between operations: each kernel polls the
evaluation's cancellation token per contour, segment, anchor, dash, dot, sample and vertex, and between
calls into the polygon library. The polygon library's own calls cannot be interrupted; the Path caps
bound each of them, and the kernel polls immediately before and after. A cancelled kernel answers
shaped empty paths with a cancelled outcome, and the evaluator stores nothing in the memoization cache
for it, so a later evaluation recomputes. `ValueUtilityLimits::pollObserver` is a test seam that
counts or triggers cancellation at a chosen poll.

## Evidence

- CPU: `bloom.runtime.path_processing` covers cancellation at the first, a middle and the last polls of
  all nine kernels (and that at most a few polls follow a request), the exact budget diagnostics, each
  brush stamp and Mask Path and Text Outline's own refusals.
- Sources: `bloom.runtime.path-source-nodes` compares Mask Path and Text Outline, through the real
  compiler and CPU evaluator, bit for bit with the same path authored on a Path node: all of a layer's
  masks, one of them, a keyed mask between and at its keys, a Mask node, a stroked mask, a filled and a
  stroked text, a keyed text size, a driven text, nothing to read, and an undone and redone source edit;
  and the filled outline merged with its text covers exactly the letters.
- Persistence: `bloom.project.path-nodes` saves and reopens every path and analysis node byte
  identically and undoes and redoes operand edits, stamps and sources included;
  `bloom.runtime.path-stroke-stamp-migration` opens a Path Stroke saved before stamps with them bound
  at Dot, Automatic, 0 and renders it bit-identically.
- Device: `bloom.runtime.gpu_path_processing_native` renders each kernel through the production GPU
  scene path against the CPU reference, cold and warm, with operand-edit invalidation, unrelated-edit
  no-ops, a keyed Trim Path end, HDR and alpha, a data window, and a region of interest; each stamp
  shape and alignment; a stroked layer mask and a stroked text outline; an image effect or recipe
  (Gaussian Blur, Exposure, Drop Shadow, three Glows, two Alpha Contours) over a curved Shape Source
  and over a Text Source; Path Stroke's core through Glow; and every glowing path preset. It exits 77
  (skipped) with no device; a skip is not a pass. It found and fixed two CPU/GPU disagreements: a
  zero-area shape leaf (a trimmed straight line) now publishes its bounds on the GPU as on the CPU, and
  a recipe over a vector leaf (Glow over a Shape Source) now keeps its halo, the leaf's window being
  the effect's output window. `--montage <dir>` writes every case's CPU and GPU frame for review.
- Interactive routes: `bloom.ui.gpu_ram_preview_path_nodes` drives a Path Stroke over a Trim Path in
  the viewer (GPU-resident, no CPU fallback), a cold RAM preview, a warm one with no dispatch, and an
  operand edit that invalidates exactly the cached frames.
- Export routes: `bloom.ui.viewer_export_path_value` also renders a composition carrying the #509
  features (the keyed path stroked as Diamond stamps and outlined by Alpha Contour, a text stroked
  along its Text Outline in a Merge, a mask stroked through Mask Path, and the Saber preset) through
  the Viewer and the still, PNG range and movie exports, on the CPU reference and on the device; every
  exported frame matches the Viewer's, and on the device every one was evaluated there.

## Transform Path

`bloom.transform-path`. Operands `path`, `position` (0, 0), `anchor` (0, 0), `scale` (1, 1, a multiplier shown as a percentage, as in Transform),
`rotation` (0, degrees, positive clockwise on screen), `skew` (0, degrees, within 89.9 either way) and
`skewAxis` (0, degrees clockwise from screen right).

Each point and handle maps as `position + R(rotation) * K(skew, skewAxis) * S(scale) * (point - anchor)`
with `S(scale)` the multiplier and `K` the slant: a point slides along the skew axis by `tan(skew)`
times its distance across that axis (skew 45 along the x axis moves each point right by its height).
A cubic maps to the cubic of its mapped controls, so the result is exact for curves, and a pure matrix
never adds a point. Contour order, closed flags, anchor modes, the fill rule and the groups are kept, so a
subpath keeps its identity. A skew of zero is arithmetically the transform this node always did: the
slant step is skipped, so older results are the same bytes.

**Skew arrived after the node.** A Transform Path saved before skew has no `skew` or `skewAxis`
parameter; it compiles and reads no skew, and its Properties show no skew rows, so to slant one, add a
new Transform Path (the same rule Transform 2D's skew follows). A new node has both, keyable like
Warp Path's amount.

**The shared matrix.** The six matrix operands are named and meant the same on Transform Path and
Warp Path. Wire one set of value nodes (a Vector2 for position, anchor and scale, a Scalar each for
rotation, skew and skew axis) to the sockets of any number of either node and every path moves in the
same space, whatever is keyed. This is how paths stay aligned until the named coordinate spaces of #591
exist; they will take over from it. The matrix lives in `src/runtime/path_warp.hpp` (`Matrix`).

## Trim Path

`bloom.trim-path`. Operands `path`, `start` (0), `end` (100) and `offset` (0), all percent of length;
selector `mode`: Simultaneously (default) or Individually. Outputs `path` and `length` (the length of the
whole input, in pixels).

The stretch from `start` to `end` is kept; `offset` slides it along. A start past the end is read the
other way round. When every contour in the scope is closed the stretch wraps past the end and continues
from the start, and a wrapped stretch of one closed contour is one open contour; an open contour clamps
instead. A range of 100 percent is the whole path and answers the operand itself. Simultaneously measures
all contours as one length (in order); Individually trims each against its own.

Lengths are measured on 64 chord samples a curved segment (relative error below 3 in 100,000 for a
quarter circle) and exactly for a straight one; the cut itself is exact (de Casteljau). Groups follow
their contours: a contour trimmed away takes its group's key with it, per the merge rule in
path-value.md.

## Round Corners

`bloom.round-corners`. Operands `path` and `radius` (10 pixels). Every corner anchor with no handles
whose two sides are straight becomes two anchors joined by the circular arc of that radius, as a cubic
with handles `4/3 * tan(turn / 4) * radius` long. The curve is held to half of each side, so neighbors
never overlap. The ends of an open contour and any anchor with a handle are left as they are. A radius
of zero or less answers the operand itself. Contours, groups and the fill rule are kept.

## Offset Path

`bloom.offset-path`. Operands `path`, `amount` (10 pixels, positive grows), `miterLimit` (4) and
`tolerance` (0.25); selector `join`: Miter (default), Round or Bevel.

The closed contours are flattened at `tolerance`, their overlap and winding settled under the value's
fill rule (a value that names none is read as non-zero), and the filled area is offset, so a ring's hole
shrinks as its outer edge grows. Open contours have no inside and are kept as they are. The result is
line segments, one corner anchor per vertex, with the even-odd rule, and its groups are dropped. An
amount of zero answers the operand itself.

## Boolean Paths and Merge Paths

`bloom.boolean-paths`: operands `first`, `second`, `tolerance`; selector `operation`: Union (default),
Subtract (first less second), Intersect, Exclude. Each side is a filled shape under its own fill rule.

`bloom.merge-paths`: operands `path`, `tolerance`; the same selector. Each contour is one shape and the
first is combined with each later one in order (Union is every contour at once). An open contour fills as
though closed.

The result is line segments at `tolerance`, even-odd, and has no groups. Both write their coordinates at
a thousandth of a pixel.

## Path Stroke

`bloom.path-stroke` (#509). Operands `path`, `width` (8), `start` (0), `end` (100), `offset` (0),
`taperStart`, `taperEnd` (0, percent of the drawn stretch), `dashLength` (0 is solid), `dashGap` (8),
`dashOffset`, `spacing` (0 is continuous; above 0 the brush stamps round dots of the width every
`spacing` pixels), `wiggle` (0, pixels), `wiggleFrequency` (2, cycles per 100 pixels), `wigglePhase`,
`seed`, `coreWidth` (0, percent of the width) and `tolerance`; selectors `cap` (Butt, Round default,
Square) and `join` (Miter, Round default, Bevel). Outputs `path` (the body), `core` and `length` (the
drawn length before dashes).

The stroke is answered as the closed outline it covers, one polygon union of a trapezoid per segment,
joins and caps, so Shape Source's production fill draws it on every route; the node itself produces no
pixels. Write-on is `end` animated; a taper narrows the stroke to a point at the drawn end, so it follows
the tip. A wiggle moves points along the normal by smooth noise that is a function of the distance along
the path, the seed and the phase (animate the phase to make it boil). `core` is the same stroke at its
share of the width: draw it brighter over the body for a beam. Glow itself is not part of the node; blur
the body with an image node. Counts of dashes, dots and wiggle samples are bounded, and a result beyond
them fails with a diagnostic. The outline's groups are dropped.

### Brush stamps

With `spacing` above 0 the brush stamps a shape every `spacing` pixels instead of drawing a continuous
stroke. Selector `stamp`: Dot (default; the width across, the round dot every stroke stamped before
stamps existed), Square (side the width), Diamond (diagonal the width) or Dash (the width long and a
quarter of it thick). Selector `stampAlign`: Automatic (default: a Dash follows the path, the others do
not), Follow path (each stamp turns with the path's direction where it lands) or Fixed (each stamp keeps
the screen's axes). Operand `stampRotation` (degrees, clockwise on screen) turns every stamp further.
A taper scales each stamp with the width there. The three are optional: a Path Stroke saved before
them opens with them bound at Dot, Automatic and 0 and draws exactly what it drew
([project-format.md](project-format.md), "Path Stroke Stamps").

### Glowing strokes

The stroke is an outline; its glow is an image effect over the Shape Source that fills it, so the glow
keeps every Glow control (and Glow's GPU route). A glowing core is the body filled in a colour with its
`core` (or a white fill) drawn over it, the two glowed. The Add menu's Light & Shadow > Glowing paths
makes this ready-made as a Node Group (Saber, Beam, Vegas, Write-on, Stroke; `CreatePathGlowPreset` in
src/commands): a group definition holding a Path Stroke of its own path, a Shape Source filling the
stroke and a Glow with no luma threshold, and an instance whose slots carry the stroke Width, the
glow's Size and Intensity and the colour (Saber: a white core in a tinted glow; the others fill the
stroke in the colour and glow it). Write-on's End and Vegas's Dash Offset run with time inside the
group (a Value Time times a rate). A group slot has no Path kind and cannot be keyed, so the path and
that motion are edited by opening the group; once made, a preset is an ordinary group. The instance's
slots are named after what they drive ("Path Stroke Width", "Glow Size") on its card and in Properties.
Each preset is laid out for a 1080-pixel-high frame and scaled to the composition it is added to.
`bloom.runtime.path-glow-presets` proves each preset is one undo step, lights the frame, follows its
Width, moves (Write-on, Vegas), and saves, reopens and renders bit-identically.

## Mask Path

`bloom.mask-path` (#509). Selector `source`: a Layer node (its masks; a track matte's mask is not one)
or a Mask node (its one path), by node id, 0 for the composition's first that has one. Operand `mask`
(0): 0 answers every mask, one after another in mask order; n answers the nth. Outputs `path` and
`count` (contours). Each mask is one contour (a mask is a simple path) and the result keeps the first
mask's fill rule. A keyed mask path is read at the frame, so the node is time dependent exactly when
the mask is. A source with no mask, or none chosen in a composition without one, answers the empty
path; a `mask` below 0 fails with a diagnostic.

The path is in the masked layer's own space, the space its masks are drawn in. A Shape lies on the
masks when its layer maps that space as the masked layer does; see "Stroking in place" below, which
sets that up in one step.

## Text Outline

`bloom.text-outline` (#509). Selector `source`: a Text Source by node id, 0 for the composition's first.
Outputs `path` and `count`. The text's glyph outlines, laid out as the text draws them (its content,
size, line height, letter spacing, alignment, box, wrap, vertical alignment, anchor and overflow, read
at the frame, so keyed or driven content and size follow), in the text's own space, filled by the
non-zero rule as glyphs are. A Shape drawing it lies on the letters when its layer maps that space as
the text's layer does: merged with the text before the text's layer (both entries of one Merge at their
defaults), or on a layer of its own set up as "Stroking in place" below describes.
`bloom.runtime.path-source-nodes` proves the Merge case: a Shape filling the outline hides the text
exactly, and the text hides the fill exactly, in either stacking order. The outlines come
from the same glyph source the text rasterizer reads: font shaping is the host preparation the GPU
contract already names, so the node adds no pixel operation. A text beyond the Path value's contour or
anchor caps fails with a diagnostic ("shorten it"); nothing to read is the empty path.

## Stroking in place

A Shape on a Layer of its own, at that layer's defaults, does not lie on the letters or masks: a
Layer node pivots about its own content's centre, which is the stroke's centre, not the text's, and
its position is the frame's centre, not the text layer's. Text & Paths > **Stroke the Selected Text**
and **Stroke the Selected Layer's Masks** (`commands::StrokeLayerOutline`, one undo step) set up the
stroke so it lands without manual steps: a Text Outline (or Mask Path) reading the selected layer's
text (or masks), a Path Stroke, and a Shape Source in a new Merge entry directly above the selected
layer's entry. That entry's own layer is at the identity (pivot Frame at its defaults) and is
**parented to the selected layer**. A child layer places its content in its parent's own space through
the parent's whole transform (layer-graph-model.md, "Transform Parenting"), and the parent's own space
is the space the text and masks are drawn in, so the stroke stays on them whatever the layer's
position, anchor, scale, rotation and parents are, keyed or not. The new layer keeps the selected
layer's in and out points. A Merge entry's own layer with masks is first given a Layer node (in the
same step, the picture unchanged), because Mask Path reads a Layer node's masks. The text is found on
the layer's image chain (its content, or an image effect's first Image input above it).

`bloom.runtime.path-source-nodes` proves it for a text on a Layer node, a masked solid on a Layer node
and a masked Merge entry's own layer, each moved, scaled and turned: the setup is one undo step, the
stroke covers every partly covered edge pixel of the text or the masked layer, the outline filled on
the stroke's layer covers exactly what the source layer shows (no pixel clearly covered by one and
clearly uncovered by the other), and with the parent removed the same fill lands elsewhere.
`bloom.ui.node-canvas-polish` drives the menu: disabled with its reason until a layer that shows text
is selected, then the stroke on its own parented layer, undone in one step.

## Where a path comes from: the boundary

Mask Path and Text Outline read what is drawn as vectors. An outline of pixels (a matte's edge, an
image's alpha) is not traced into a path: that would read the frame's pixels back to the host mid-frame,
which the GPU contract forbids. Alpha Contour ([image-filters.md](image-filters.md), "Alpha Contour")
draws the band along an image's alpha edge in image space instead, with a width, softness and offset;
the path-only controls (trim, dash, taper, stamps, wiggle) belong to vector paths and do not apply
there.

## Resample Path

`bloom.resample-path` (Text & Paths) answers the same path with its points placed along each contour:
more points, fewer, or evenly spaced. It takes a Path operand and answers a Path, evaluated every frame
from the operands of that frame. It adds no image lowering and needs no GPU coverage exception: the path
it answers is drawn by Shape Source's production fill.

| Operand | Meaning | Default |
| --- | --- | --- |
| Mode | Fixed count, Gap, By length or Percentage | Fixed count |
| Count | Points per contour (Fixed count) | 32 |
| Gap | Largest distance between neighbours, in pixels (Gap) | 10 |
| Density | Points per 100 pixels of length (By length) | 10 |
| Percentage | Share of the contour's own point count (Percentage) | 100 |
| Keep corners | Existing corners stay as points | on |
| Output | Straight segments or Curves | Curves |
| Start offset | Slides the points along the contour, 0 to 100 percent | 0 |

Every contour is resampled, open or closed, and the groups are kept. A closed contour has no duplicate
point at its seam; Start offset wraps around it. An open contour always keeps its two ends and Start
offset slides its inner points by up to one gap. Gap places the fewest evenly spaced points whose spacing
does not pass the gap (a 100 px line at gap 10 gives 11 points); By length is Gap with a gap of 100
divided by the density. A corner is an anchor where the direction of travel turns by more than half a
degree; with Keep corners on, each span between corners is spaced on its own, so a square keeps its four
corners. A count smaller than the corners need keeps the corners and ignores the rest.

Curves are cubic segments through the new points that follow the original contour: a span inside one
original segment is the exact sub-curve, and a span over several is a least-squares fit. Their
deviation from the original is at most 0.25 px at the original density or finer. Straight joins the new
points with lines.

**Controls.** The card and Properties show the operands the mode reads: Count for Fixed count, Gap for
Gap, Density for By length, and a slider from 0 to 200 percent for Percentage. Start offset is a slider
from 0 to 100 percent, Keep corners a switch, and Mode and Output are choice lists. A cached Resample
Path result is keyed by its operands and by the keys of the Path node that feeds it, so editing either
draws a new picture. Benchmark scenario: `resample-path-2k` in `tools/bench` (written, not run).

**Across frames.** Fixed count gives the same number of points on every contour at every frame, so two
paths resampled to the same count have the same structure and interpolate (the path animation rule in
path-value.md). Gap, By length and Percentage follow the path's length or point count, so the count may
change from frame to frame, and such paths do not interpolate with each other.

**Limits.** A contour holds at most 4096 points and a value 262144. A request beyond either, from any
mode, is not clamped: the node fails with a diagnostic naming "Resample Path", the mode and the number
requested, and answers the empty path. A count under 2 (open) or 3 (closed) is raised to that minimum.
A contour of length zero answers copies of its point for Fixed count and is reported as a notice, not a
failure. The result's anchors are charged to the evaluation's retained-path budget. Cancellation is
observed between contours.

The arithmetic (`src/runtime/path_resample.hpp`) is shared: the morph (#597) gives two contours the same
number of points with `resampleToCount`. Warp Path subdivides by curve parameter and not by length (see
Warp Path), so it can follow a Resample Path in a chain but does not call it.

### Write to a Path

The command `bloom.node.write-to-path` (node context menu "Write to a Path", MCP, Python
`bloom.ops.node.write_to_path`, and the same operation id in a `bloom-cli run` script) reads what any Path-producing value node answers at the playhead and
writes the points into a Path node: a new one beside the source, or an existing constant, or a whole-path
key at that time on an animated one. A Path node has no input socket, so its value is always a constant or keyed. It is one undo step, and the groups
and fill rule are kept. The read runs off the UI thread and is cancellable.

## Warp Path

`bloom.warp-path` (Text & Paths) bends a path through a non-linear function of its points: a straight
line cannot be bent by a transform or a skew, but it can by `(x, y) -> (x + A sin(y / w), y)`. It takes a
Path operand and answers a Path, evaluated every frame from the operands of that frame. It adds no image
lowering and needs no GPU coverage exception: the path it answers is drawn by Shape Source's production
fill, which has CPU/GPU parity on every route.

**Separate node.** Warp Path is its own node and not a mode of Transform Path (the choice #598 left
open): Transform Path keeps its operands and its exact-curve guarantee, which older projects rely on,
and the warp needs operands and a tolerance that a pure matrix does not. The affine matrix, with its
skew, stays on Transform Path, and both nodes share the matrix operands (above).

| Operand | Meaning | Default |
| --- | --- | --- |
| Function | Wave, Twist, Bulge / Pinch, Bend or Ripple | Wave |
| Amount | How far: pixels (Wave, Ripple), degrees (Twist, Bend), percent (Bulge / Pinch); a track from -360 to 360 | 20 |
| Size | The width the function works over, pixels, above 0 | 100 |
| Phase | Where in its cycle, degrees (Wave and Ripple only) | 0 |
| Tolerance | The farthest, in pixels, the drawn curve may stray from the exact warp, 0.001 to 1000 | 0.25 |
| Position, Anchor, Scale, Rotation, Skew, Skew axis | The warp's space: Transform Path's matrix | identity |

The function runs in the space the matrix describes: a point goes into that space by the matrix's
inverse, through the function, and back out by the matrix, so the wave, twist or bend turns, slants,
stretches and moves with the space (a scale of zero cannot be inverted and is refused). In the space, with
`A` the amount, `w` the size and `p` the phase:

| Function | Point `(x, y)` goes to |
| --- | --- |
| Wave | `(x + A sin(2 pi y / w + p), y)`: vertical lines become waves |
| Twist | the plane turned about the origin by `A` degrees times `exp(-r^2 / w^2)` (`r` the distance from the origin) |
| Bulge / Pinch | `(x, y)` times `1 + (A / 100) exp(-r^2 / w^2)`: out for a positive amount, in for a negative one |
| Bend | the x axis curled into an arc turning `A` degrees over a length of `w`; `y` runs along the arc's radius |
| Ripple | out along the line from the origin by `A sin(2 pi r / w + p)` |

**Adaptive subdivision.** A warped straight segment is a curve, so every segment is split until the curve
fits. Each stretch of a segment, between two parameters, is drawn as a cubic through the exact warped
end points with the warped tangents (a central difference of the warp of the segment) as handles. The
stretch is accepted when that cubic is within the tolerance of the exact warp at three parameters inside
it (a quarter, a half and three quarters of the way), otherwise it halves. Every output point is exactly
the warp of a point of the original (an anchor or a subdivision knot), anchors keep their own corner or
curve (the tangent comes from the segment that leaves or arrives), and a segment the warp only moves,
such as a horizontal line under a Wave, stays two points. The subdivision is in the segment's own
parameter, so a path chained after Resample Path keeps the spacing Resample gave it; it does not need
Resample's arc-length helper.

The tolerance is checked at those three parameters, not proved everywhere: the tests measure the whole
curve against the analytic warp (largest deviation at most the tolerance for a vertical grid under a Wave,
a circle under a Ripple and a line under a Bend).

**Limits.** A stretch splits at most 14 times along any one chain; one that is still outside the
tolerance there (the warp jumps, as a Ripple does through its own origin off a whole cycle) is kept and
reported as a notice, not a failure. A contour holds at most 4096 points and a value 262144: a warp that
needs more fails with a diagnostic naming "Warp Path", the function, the tolerance and the least number of
points it needs, and answers the empty path (raise the tolerance, lower the amount or widen the size).
A size at or below 0, a tolerance outside its range, a skew past 89.9 degrees, an operand that is not
finite and a coordinate beyond the bound fail the same way. Contour count and order, closed flags, the
fill rule and the groups are kept, one contour to one contour. The result's anchors are charged to the
evaluation's retained-path budget. Cancellation is observed between contours and every 64 steps inside
one, and a cancelled evaluation keeps nothing.

**Across frames.** The operands are read every frame, so an amount, phase or skew keyed on the node (or
a value node wired to any operand) animates the warp, and an animated upstream path is warped at each
frame. The count of points can change from frame to frame with the curve, so a warp's keys do not
interpolate with each other the way a Fixed count Resample does; resample first when two warps must
share a structure.

**Controls.** The card and Properties show a pad with Position and Anchor, dials for Rotation and Skew
axis (clockwise on screen), tracks for Amount and Skew, then Size, Phase (for Wave and Ripple), Scale and
Tolerance. Properties list Path, Function, then the card, then the pad's X and Y rows. Amount, Size,
Phase, Tolerance, Skew and Skew axis can be keyed on the node and wired; Position, Anchor, Scale and
Rotation follow the value library's rule and are animated by wiring keyed value nodes to them.
Benchmark scenario: `warp-path-2k` in `tools/bench` (written, not run).

### Warp Path (Function)

Warp Path (Function) (`bloom.warp-path-function`, Text & Paths) bends a path with a group's function, the
custom case of #598. It is a function consumer like Function Plot: it names one group (scalar in, scalar
out) and reads `f` through the same evaluator. A point `(x, y)` goes to `(x + amount * f(y / size), y)` in
the matrix space (Position, Anchor, Scale, Rotation, Skew and Skew axis, as on Warp Path), so a group
holding Sine is the issue's `(x + sin y, y)`. Rotate the space by 90 degrees to displace y by x, or chain
two nodes to do both; this is the recorded default in place of a second function slot. The same adaptive
subdivision, tolerance, limits and cancellation as Warp Path apply. A node naming no group does not
compile. A group that has no answer at a point of the path (1 / x at zero) or answers a value that is not
finite stops the warp with a diagnostic on the node and answers the empty path. Tests:
`testWarpPathFunction` in the snapshot compiler tests (exact anchors, deviation from the analytic curve,
no-answer refusal, unset group).

## Reference library

Offset, union, difference, intersection and exclusive-or use a vendored polygon clipping library, the
established reference for them, compiled Qt-free into the runtime (`src/runtime/third_party/clipper2`,
license and provenance records under `dependencies/licenses/clipper2`). No Bloom header names one of its
types. Flattening is the render library's `flattenPathContour`, the one flattening every route shares.

## Groups

Resample Path, Warp Path, Transform Path and Round Corners keep every contour and so every group. Trim Path keeps the groups of
the contours it keeps. Offset, Boolean and Merge answer geometry that does not descend one to one from
contours, so they answer no groups; a consumer tracking a key finds it gone, as path-value.md allows.
