# Path Processing

Status: built (#344, #530, #509). This document owns the nodes that take a Path and answer a Path
(Transform Path, Trim Path, Offset Path, Boolean Paths, Merge Paths, Round Corners and Path Stroke) and
the two that read a Path from what an artist already drew (Mask Path and Text Outline). The value they
work on is [path-value.md](path-value.md). They live in Text & Paths and are value nodes, so they appear
in a value graph and feed Shape Source's `path` socket like any other Path producer.

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

`bloom.transform-path`. Operands `path`, `position` (0, 0), `anchor` (0, 0), `scale` (1, 1, a multiplier shown as a percentage, as in Transform)
and `rotation` (0, degrees, positive clockwise on screen).

Each point and handle maps as `position + R(rotation) * S(scale) * (point - anchor)` with `S(scale)` the multiplier. A cubic maps to the
cubic of its mapped controls, so the result is exact for curves. Contour order, closed flags, anchor
modes, the fill rule and the groups are kept, so a subpath keeps its identity.

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

The path is in the masked layer's own space, the space its masks are drawn in. For a frame-sized layer
at its default place that is the frame's, so a Shape drawing the stroke as an entry of the same Merge at
the entry's defaults (the identity) lies on the masks. A Shape on a Layer of its own pivots about its own
content's centre and does not.

## Text Outline

`bloom.text-outline` (#509). Selector `source`: a Text Source by node id, 0 for the composition's first.
Outputs `path` and `count`. The text's glyph outlines, laid out as the text draws them (its content,
size, line height, letter spacing, alignment, box, wrap, vertical alignment, anchor and overflow, read
at the frame, so keyed or driven content and size follow), in the text's own space, filled by the
non-zero rule as glyphs are. A Shape drawing it lies on the letters when it is merged with the text
before the text's layer (both entries of one Merge at their defaults, the Merge the layer's content),
so the layer moves both; a Shape on a Layer of its own pivots about its own content's centre and does
not. `bloom.runtime.path-source-nodes` proves it: a Shape filling the outline hides the text exactly,
and the text hides the fill exactly, in either stacking order. The outlines come
from the same glyph source the text rasterizer reads: font shaping is the host preparation the GPU
contract already names, so the node adds no pixel operation. A text beyond the Path value's contour or
anchor caps fails with a diagnostic ("shorten it"); nothing to read is the empty path.

## Where a path comes from: the boundary

Mask Path and Text Outline read what is drawn as vectors. An outline of pixels (a matte's edge, an
image's alpha) is not traced into a path: that would read the frame's pixels back to the host mid-frame,
which the GPU contract forbids. Alpha Contour ([image-filters.md](image-filters.md), "Alpha Contour")
draws the band along an image's alpha edge in image space instead, with a width, softness and offset;
the path-only controls (trim, dash, taper, stamps, wiggle) belong to vector paths and do not apply
there.

## Reference library

Offset, union, difference, intersection and exclusive-or use a vendored polygon clipping library, the
established reference for them, compiled Qt-free into the runtime (`src/runtime/third_party/clipper2`,
license and provenance records under `dependencies/licenses/clipper2`). No Bloom header names one of its
types. Flattening is the render library's `flattenPathContour`, the one flattening every route shares.

## Groups

Transform Path and Round Corners keep every contour and so every group. Trim Path keeps the groups of
the contours it keeps. Offset, Boolean and Merge answer geometry that does not descend one to one from
contours, so they answer no groups; a consumer tracking a key finds it gone, as path-value.md allows.
