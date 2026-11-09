# Path Value

Status: built (#589). This document owns what a path is once it travels through the graph: the value,
its socket, its keys, how it animates, how it is subdivided and measured, and how older projects open.
The code that owns each part names this document. Path effects, merges and generators (#343, #344,
#345, #509, #529, #530, #526) are built on this value and are not part of it.

## The value

`document::PathValue` is an ordered list of contours, a fill rule and keyed groups:

- A `document::PathContour` is its anchors and its own closed flag. An anchor is unchanged: a point,
  two optional handles and a handle mode.
- `fillRule` is `Inherit`, `NonZero` or `EvenOdd`.
- `groups` names runs of contours (see [Keys](#keys-on-groups-of-contours)).

The contours are the whole shape together: holes come from the fill rule, not from a second operation.
Contour order is authored and significant to groups and to interpolation.

A value holding at most one contour, with no fill rule and no groups, is **simple**. Everything that
existed before compound paths holds a simple value, and `PathValue::fromContour` builds one.

### Which fill rule wins

The value's explicit rule wins over Shape Source's `fillRule` parameter. `Inherit`, the default and
what every project older than document 1.43 holds, means "use the parameter". So a Shape Source whose
path has no opinion behaves exactly as it always has, and a producer that needs a rule for its contours
to mean what it intends (a hole cut by even-odd) cannot have that undone downstream.
`detail::shapeFillRule` in `src/runtime/shape_geometry.hpp` is the one place both evaluators ask.

### Caps

The anchor cap stays **per contour**: `kMaximumPathAnchors` is 4096, as it was for the single contour
a path used to be. The whole value is bounded separately so a compound value cannot defeat the bound:

| Limit | Value |
| --- | --- |
| Anchors in one contour (`kMaximumPathAnchors`) | 4096 |
| Anchors in one value (`kMaximumPathValueAnchors`) | 262144 |
| Contours in one value (`kMaximumPathContours`) | 65536 |
| Groups in one value (`kMaximumPathGroups`) | 65536 |
| Bytes in one group key (`kMaximumPathGroupKeyBytes`) | 256 |

The per-value anchor bound is the same bound `PathRaster` applies to the segments it will flatten, so
any valid value is one it can be asked to draw.

### Who reads a compound value

Only the consumers that have a meaning for contours accept them. Shape Source and the Path node do. Each
other path surface stays one contour, and validation refuses a compound value, a fill rule or groups
there rather than quietly using the first contour:

| Surface | Holds | Why |
| --- | --- | --- |
| Shape Source `path`, Path node, data block | Any valid value | Fill and stroke read all contours |
| Layer mask `path` | Simple | A mask's feather, expansion and handles are defined for one outline |
| Curves, field remap, shutter and depth fog curves | One open contour | A curve is a function of its input |

A curve point is a plain point: an anchor with no handles. An Auto anchor whose handles are null is
accepted as one and stored as Corner (its handles would be derived, and a curve is interpolated
through its points whatever the mode), so MCP, Python and scripts may send either spelling. A curve
anchor with a handle, a Mirrored, Aligned or Free mode, a point outside the curve's domain, a closed
or compound value is refused naming the anchor, the shape a curve takes and what was received.

The viewer's pen and anchor tools edit simple values. A compound value on a Shape Source is drawn and
shown in Properties (as its contour and anchor counts) but not edited, so no tool can silently drop a
contour.

### Filling and stroking several contours

`PathRaster::create` over several contours fills them together under the fill rule and strokes each.
An open contour fills as though closed by a straight line, as it always has. Inside and Outside stroke
alignment need an inside everywhere, so any open contour in the value centers the whole stroke, the
same rule a one-contour open path follows.

One contour takes exactly the path it took before: `PathRaster::create` over a span of one calls the
single-path form, and the operation key of a simple value is byte for byte the key it had.

## The socket

`SocketValueKind::Path` is a value socket, with its own palette color (`docs/ux/visual-language.md`).
Nothing promotes into or out of it.

- **Evaluated per frame.** A value graph output yields `runtime::CompiledPathValue`. A constant lowers
  to one shared immutable value; a keyed one lowers to `CompiledValuePathCurve`, which samples at the
  frame's time like every other animated value.
- **Several consumers.** The output's value is held by a shared pointer in the value graph's ordinary
  output table, so any number of readers share one evaluation and no reader copies it.
- **Budget.** A path is the first value whose size is not fixed. The evaluation charges each path
  output's anchors against `maxRetainedPathAnchors` (eight full values per evaluation), and exceeding
  it fails that evaluation with the diagnostic "Value graph path budget exceeded" rather than growing.
  The operation cache charges a retained path's bytes.
- **Cache identity.** `OperationKey` hashes the resolved value, every contour, the fill rule and every
  group, with a version prefix that keeps a simple value's bytes unchanged. A shape's coverage key is
  built from the path it resolved this frame, so a driven path that changes invalidates exactly the
  frames it changes and a path that holds across frames reuses them.
- **Time dependence.** A plan whose shape reads a keyed path is time dependent through the ordinary
  value-graph analysis (`plan_time_dependence.cpp`).

Shape Source's `path` socket is the one consumer in this document: connect a path output and it
replaces the parameter's own value, and the shape's other parameters do not change meaning. The socket
shows on a Path shape, like the parameter it drives; on another kind a connection is kept and shown but
not read. The parameter itself stays a constant or a driver: keys belong to the Path node. A document
older than 1.43 cannot hold a driver there.

The **Path** node (`bloom.value-path`, in Values) is the producer: it holds a whole path value,
constant or keyed. It generates nothing and exists to prove and use the plumbing. Generators arrive
with their own issues and output this same socket.

**Function Plot** (`bloom.function-plot`, in Text & Paths) is the first generator: it samples a group's
function into one open contour per run of valid samples (value-node-library.md, **Function Plot**).

## Keys on groups of contours

A `document::PathGroup` is a key, a run of contours (`firstContour`, `contourCount`) and optional
attributes, of which `color` is the first. Groups ascend, do not overlap, have unique keys and need not
cover every contour; a contour outside every group simply has no key.

A key is author-chosen text, or derived from a source span with `pathGroupKeyFromSpan(begin, end)`
(which spells `span:<begin>:<end>`), so a generator that outlines a run of text can name the outlines
by the characters they came from. Keys compare as bytes. `matchPathGroups(from, to)` answers the pairs
two different paths share. A group's position may differ between the two paths, because the key is the
identity, not the index.

Color is stored for the consumers that will read it. No current consumer paints by group.

## Animating a whole path value

A path parameter's keys are whole values. Between two keys, `samplePathCurve` interpolates only when
`pathKeysInterpolate` says the two have the same contour count and, contour by contour, the same anchor
count and closed flag. Otherwise it **holds the earlier key** until the next one. That is the same rule
one contour always had, read across all contours. Interpolation pairs contours and anchors by index;
morphing anything beyond this is #597.

While interpolating, the anchor points and handles move; the fill rule, groups, group attributes,
closed flags and anchor modes are the earlier key's.

## Keys when contours merge

No operation in this document merges contours. The answer here is what #530 and #344 build on:

- A merged contour belongs to the group of its **lowest-index input contour**.
- That group survives with its key and attributes, covering the merged contour in place of its inputs.
- A group all of whose contours were consumed into another group's contour loses its key.
- Contours the merge does not touch keep their groups, and a merge that spans a group boundary does not
  invent a new key.

So a key never appears from nowhere, and a consumer tracking a group across an operation either finds it,
at the contours that descended from it, or finds it gone.

## Subdivision

`document::subdivideContourSegment(contour, segment, t)` splits one segment at `t` by de Casteljau on the
segment's own control points. The two new segments are exactly the two parts of the old curve, and a
straight segment stays straight with no handles and a Corner anchor. On a curve the new anchor is Aligned,
and each neighbor's handle on the split segment moves to its de Casteljau point. The viewer's add-anchor
gesture is this same function. It refuses a contour already at the cap rather than dropping anchors.

## Arc length

`render::PathArcLength` measures a contour on the same polyline `PathRaster` strokes: both call
`flattenPathContour` at the same `pathFlatteningTolerance`, which is at most 1/32 output pixel at the
requested scale. So a position along a path, a dash or anything else parameterized by distance agrees with
the drawn stroke to within that tolerance and repeats exactly. `length()` is the polyline's length;
`pointAt(distance)` clamps to the ends and returns the point at that distance; a closed contour measures
its closing segment. A straight-line distance inside the parameterization uses `sqrt`, not `hypot`. The
flattener keeps its own existing subdivision arithmetic, so one-contour rendering stays byte identical.

## Persistence

Document 1.43 (`docs/architecture/project-format.md`, "Compound Paths In Document 1.43") keeps the
existing `{kind, anchors, closed}` spelling for every value a one-contour path can express, and writes
`{kind, contours, fillRule?, groups?}` otherwise. One value has one spelling. An older project opens with
one contour wherever it had a path, on shapes, masks, data blocks, field and curve parameters and keys,
and its re-encoded document is the same bytes apart from its minor.

An empty open flat path decodes as zero contours, the empty value, so no consumer sees a contour with
nothing in it that the older build did not have.

## GPU

A shape's coverage goes through the same `PathRasterCoverageGeometry` native producer whether its path
is a constant or driven, and however many contours it has: the contours flatten once on the host
(parameter and geometry resolution, as before), and the coverage is rasterized natively. The contract
axes `feature.path.compound` and `feature.path.driven` in `gpu_coverage_contract.hpp` are Required with
genuine fixtures, and `tests/integration/gpu_path_value_native_tests.cpp` proves parity against the CPU
reference, cold and warm, cancellation and budget, with a real device.
