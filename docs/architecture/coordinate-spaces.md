# Coordinate Spaces

Status: built (#591). This document owns the coordinate space: the value, its socket, the nodes that
define, read and use one, how a space reaches pixels, and its cache identity. The code that owns
each part names this document.

A coordinate space is a named map from a space's own units to composition pixels. A graph is the
everyday case: "x from -5 to 5, y from -3 to 3, drawn here at this size". Each node that places
something in a space reads that one map, so moving or scaling the space keeps everything in it
aligned: a plot, its axes, its labels and the layers placed on it.

## The value

`core::space::Space` (`src/core/include/bloom/core/coordinate_space.hpp`) holds:

| Field | Meaning |
| --- | --- |
| `name` | The artist's name for it (`graph` by default), at most 256 bytes |
| `dimensions` | 2 for every space this release builds; 3 is reserved for #605's world and camera spaces |
| `toPixels` | The map from units to composition pixels, any parent's map already applied |
| `range` | The space's extent in its own units (a graph's plotted range), when it has one |
| `parent` | The space this one was defined in, or none for a space defined in composition pixels |

### One matrix for every kind of map

`toPixels` is one homogeneous 4x4 matrix (row-major, rows pixel x, pixel y, depth, w; columns unit
x, unit y, unit z, 1). A 2D space's 3x3 map `[a b c; d e f; g h i]` sits in rows and columns 0, 1 and
3, and its z column and depth row are the identity's. A pixel is `(row0 · p) / (row3 · p)`.

- An **affine** 2D space (origin, scale, turn) has row 3 = (0, 0, 0, 1), so its division is by
  exactly 1 and its pixels are the plain affine arithmetic, bit for bit.
- A **projective** 2D space (a plane seen in perspective, from four corners) uses g and h.
- A **3D** space (#605) fills the whole matrix with no second mechanism: the same value, socket,
  readouts and cache identity carry it.

A point is in front of the projection when its w is above 1e-12. A point at or past the horizon has
no pixel: `mapPoint` answers nothing and every consumer treats it as a gap or a refusal, never as a
pixel at infinity. The planar inverse is refused when the map is singular (its determinant is
negligible against its entries), so every space that exists can read a pixel back.

### A space has no stored form

A space is computed, never saved. The project saves the Coordinate Space node, its parameters and
its links like any other node; the space is what that node answers at a time. There is no
`ParameterValueKind` for a space and no constant of one: an unconnected space input is "no space".

## The socket

`SocketValueKind::Space`, named `space`. A space connects only to a Space input; nothing promotes to
or from one. A space input has no parameter row: it is fed only by a link, so it shows as a socket
with a label and nothing to type into. The socket is a square in Path's pale steel blue
(`SocketPath`, `docs/ux/visual-language.md`): no hue is left 20 degrees from every other socket role,
and both kinds are geometry, so the shape and the label tell them apart.

## Nodes

All five are in the Vector group of the Add menu.

### Coordinate Space

`bloom.coordinate-space` defines a space. Its `mode` chooses how:

| Mode | Reads | The map |
| --- | --- | --- |
| Origin and scale (default) | `origin`, `pixelsPerUnit`, `rotation`, `yUp` | Unit (0, 0) lands on `origin`; one unit is `pixelsPerUnit` pixels along each axis |
| Range to rectangle | `rangeMin`, `rangeMax`, `center`, `size`, `rotation`, `yUp` | The range fills a `size` rectangle centred on `center` |
| Range to corners | `rangeMin`, `rangeMax`, `topLeft`, `topRight`, `bottomRight`, `bottomLeft` | The range's corners land on the four points: a perspective plane |

| Parameter | Type | Default | Meaning |
| --- | --- | --- | --- |
| `parent` | Space (socket only) | none | The space this one is defined in; its points are then in the parent's units |
| `name` | String | `graph` | The space's name |
| `origin` | Vector 2 | (0, 0) | Pixels (or parent units) |
| `pixelsPerUnit` | Vector 2 | (100, 100) | Pixels (or parent units) per unit |
| `rotation` | Scalar | 0 | Degrees, clockwise on the frame |
| `yUp` | Boolean | on | A positive y rises on the frame |
| `rangeMin`, `rangeMax` | Vector 2 | (-5, -5), (5, 5) | The space's extent in its units |
| `center`, `size` | Vector 2 | (0, 0), (1000, 1000) | The rectangle of Range to rectangle |
| `topLeft` ... `bottomLeft` | Vector 2 | the corners of a 1000 px square from (0, 0) | The quad of Range to corners |

Properties and the card show only what the mode reads (`parameter_activity.cpp`); the compiler drops
the curves of what it does not. The map is `T(origin) · R(rotation) · S(ppu.x, ±ppu.y)` for Origin
and scale and `T(center) · R · S(size / range) · T(-rangeMiddle)` for Range to rectangle; whole
quarter turns are exact. Range to corners is Heckbert's closed-form square-to-quad map after the
range's normalization. With a parent, the map is the parent's map after this one.

The range is published whenever it runs forward on both axes; Range to rectangle and Range to
corners need one. The node is refused, with no space, when the range is empty, a number is not
finite or above 1e12, three corners are on one line, the map is flat (no size on an axis), the name
is over 256 bytes, the parent is not 2D, or the space would be more than sixteen deep.

**On the Viewer**, selecting a Coordinate Space with no parent shows its points as handles: the
origin, the rectangle's centre or the four corners, with the range's outline and one unit along each
axis drawn through the map. Each drag is one edit of one parameter, one command on release and one
undo; Escape confirms it as it stands. A space with a parent has its points in the parent's units,
so it shows no handles.

### Space Readout

`bloom.space-readout` reads a space's map as values, so anything a space does is readable and can
drive any parameter.

| Output | Type | Meaning |
| --- | --- | --- |
| `origin` | Vector 2 | The pixel unit (0, 0) lands on |
| `xAxis`, `yAxis` | Vector 2 | The pixel step of one unit along each axis there |
| `scale` | Vector 2 | The lengths of those steps |
| `rotation` | Scalar | The x axis's angle, degrees clockwise |
| `rangeMin`, `rangeMax` | Vector 2 | The range, or (0, 0) when it has none |
| `matrix` | Number List | The 3x3 map row by row for a 2D space (the 4x4 for a 3D one) |
| `name`, `parentName` | String | The names |
| `projective` | Boolean | The map has perspective |
| `valid` | Boolean | A space is connected and its origin has a pixel |

With nothing connected every readout is empty and not valid. A Coordinate Space's own `space` output
shows as short text where values are shown: its name and its scale at the origin ("graph · 40 × 30
px/unit"), with the turn and "perspective" when they apply.

### To Space and From Space

`bloom.to-space` reads a composition pixel as a point of the space; `bloom.from-space` puts a point
of the space on its pixel. `conversion` is Point (the full map, inverted for To Space) or Vector
(the map's derivative at the space's origin, so a vector turns and scales but does not move).
Outputs: `result` (Vector 2) and `valid` (Boolean). With no space, or at a horizon, the result is
(0, 0) and not valid.

### Place in Space

`bloom.place-in-space` answers what a layer needs to sit at a point of a space.

| Parameter | Type | Default | Meaning |
| --- | --- | --- | --- |
| `space` | Space (socket only) | none | The space |
| `point` | Vector 2 | (0, 0) | The point, in the space's units |
| `offset` | Vector 2 | (0, 0) | Pixels added to the placed position |
| `unitSize` | Vector 2 | (100, 100) | The pixels of one unit at which the layer is at 100% |

Outputs: `position` (Vector 2, pixels), `scale` (Vector 2, the axes' lengths at the point over
`unitSize`), `rotation` (Scalar, the x axis's angle there) and `valid`. Link them to a layer's
Position, Scale and Rotation. With no usable space the node answers the offset, 100% and no turn, so
the layer stays where its offset puts it.

### Function Plot, Axes and Axis Tick

Each has an optional `space` input (`value-node-library.md`). Connected, the space places every
point: a plot's samples, the axes and their ticks (each tick crossing its axis along the other axis's
direction there) and a tick's label position. Unconnected, they use their own `origin` and
`pixelsPerUnit`; an unturned Origin and scale space with the same numbers gives exactly the same
paths.

While something is connected to `space`, `origin` and `pixelsPerUnit` are inactive
(`document::isParameterInactive`, the rule every reader asks): Properties, the node card and the
Timeline show no row for them, and the compiler lowers them to their defaults, so setting or wiring
one changes neither the plan nor its cache identity. The rule is on the connection and the role,
because these rows share the generic Vector 2 operand key with rows that stay active (Axis Tick's
`labelOffset`); it needs no stored change, so saved projects open and draw as before. A wired
placement row keeps its row on the card so its link never dangles. A connected space that is refused
upstream reaches the node as "no space", which then places at the defaults (0, 0) and 100 pixels
per unit; the refusal is reported on the Coordinate Space.

## How things stay aligned

Every consumer reads the one map at the same time, so a change to the space reaches all of them in
the same evaluation. Two things make the pixels agree, not only the numbers:

- A path drawn in a space is in composition pixels. Draw it as a Merge entry (which pivots on the
  frame, #502) so that path pixels are frame pixels; a Shape Source drawn as its own layer would
  pivot on its content's centre, which moves when the path changes.
- A layer is placed in a space through Place in Space, whose Position output is the pixel the
  layer's centre should be on.

`tests/integration/gpu_coordinate_space_native_tests.cpp` holds the evidence. It builds a graph
scene through commands (a plot, axes, a tick label and two placed layers, all in one space whose
origin and scale are keyed) and checks, over several frames, that the readouts equal an independent
map, the placed layers' centroids are on their points, the plot and the axis are on their pixels
with empty pixels beside them, and the label moves with its mark. It then checks the same frames on
the GPU route against the CPU reference, and save and reopen, undo, cache identity, cancellation,
budget and retirement.

## Cache identity

A space is part of every operation key that reads one (`src/runtime/operation_key.hpp`): presence,
then for each level of its parent chain its name, dimensions, matrix, range and whether it has a
parent. Two spaces with the same map and names are the same identity; a change to any of them
invalidates exactly the values and pixels downstream.

## GPU

A space is a host value computation: nodes answer the same values on every route. The pixels it
places are drawn by the existing Path and layer routes, which carry their own parity, so a space
adds no image operation and no GPU coverage entry. The native test checks CPU/GPU parity at 2e-6
on every frame it reads.

## Scripting and agents

Python, JSON scripts and MCP build a space like any node: `bloom.node.add` with
`bloom.coordinate-space`, `bloom.node.set-parameter-value` for its parameters, and `bloom.node.connect`
from its `space` output to a `space` input (there is no parameter to set).

Any node's outputs are read as values at a time with MCP `query {kind:"outputs", composition, node,
time}` and Python `bloom.data.outputs(node, frame=..., time=...)`. Both call
`scripting::readNodeOutputs` (`src/scripting/include/bloom/scripting/readings.hpp`), which compiles
the plan with the node as a value readout and evaluates it on the CPU reference path off the UI
thread, stopping when the caller stops waiting. Each output is listed with its socket kind and its
value: a Space Readout's origin, axes, scale and rotation as numbers and vectors, its matrix as a
Number List, its names as text; To Space and From Space as vectors; a Coordinate Space's own output
as `{name, dimensions, matrix, affine, range, parent}`, where `matrix` is the 4x4 above as four rows.
Pictures, sound and fields are listed with no value and a note. The answer carries the revision it
read; an unknown composition or node, or a stale `expectedRevision`, is refused as other queries
refuse. `tests/fixtures/scripting/agent_reference.py` checks that MCP and Python answer the same
values for the same saved project.

## Related work

3D world and camera spaces are #605's; the value, socket, readouts and cache identity here already
carry them.
