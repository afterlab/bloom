# Path Processing

Status: built (#344, #530). This document owns the nodes that take a Path and answer a Path: Transform
Path, Trim Path, Offset Path, Boolean Paths, Merge Paths and Round Corners. The value they work on is
[path-value.md](path-value.md). They live in Text & Paths and are value nodes, so they appear in a value
graph and feed Shape Source's `path` socket like any other Path producer.

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
limit (1 to 100). Work is bounded by the Path value's caps, so one evaluation is bounded work; the
evaluation of a value graph is where cancellation is observed, between operations.

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

## Reference library

Offset, union, difference, intersection and exclusive-or use a vendored polygon clipping library, the
established reference for them, compiled Qt-free into the runtime (`src/runtime/third_party/clipper2`,
license and provenance records under `dependencies/licenses/clipper2`). No Bloom header names one of its
types. Flattening is the render library's `flattenPathContour`, the one flattening every route shares.

## Groups

Transform Path and Round Corners keep every contour and so every group. Trim Path keeps the groups of
the contours it keeps. Offset, Boolean and Merge answer geometry that does not descend one to one from
contours, so they answer no groups; a consumer tracking a key finds it gone, as path-value.md allows.
