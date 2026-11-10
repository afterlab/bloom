# Vector Artwork (SVG import)

Status: canonical

Updated: 2026-11-10

Issues: [#343](https://github.com/kinetik-gg/bloom/issues/343),
[#642](https://github.com/kinetik-gg/bloom/issues/642).

An SVG file imports as native Bloom layers, not as a picture. An artist can select any element,
move its points, key them, fill it with another colour, run Trim Path or Offset Path over it, and
the render is the production evaluation path: preview, still export, sequence export and headless
render all read the same document. This document records the design, the supported profile, the
limits and what is reported rather than imported.

## Two modes (#642)

`bloom.layer.add-svg` (`AddSvgArtwork`, `src/commands/svg_import_operations.cpp`) is one operation
and so one undo step, in one of two modes (`SvgImportMode`, spelled `layer` and `shapes`):

- **As one layer** (`layer`, the operation's default): one entry layer whose source is one
  **Vector Artwork** node (`bloom.vector-artwork`, `src/document/vector_artwork_nodes.hpp`). The
  graph holds that layer and that node, nothing else. The file's bytes are held in the project, so
  the layer renders, saves and reopens with the file gone.
- **As editable shapes** (`shapes`): the breakdown described under [Structure](#structure), every
  element its own layer and Path node.

Both modes build from one description. `buildSvgArtwork()` (`src/document/svg_artwork.cpp`,
library `bloom_svg_artwork`) reads a parsed scene and describes the nodes, Merge slots, entry
layers, layer values, track mattes and node frames in the order they are made, as plain data;
`SvgArtworkWriter` writes a description into a composition through an `SvgArtworkTarget` that
supplies the ids and makes entry layers. The editable import writes it with ids from the project in
a Draft; the Vector Artwork layer's expansion writes the same description with derived ids. Both
modes report the same named warnings at import.

### The Vector Artwork node

| | |
| --- | --- |
| Type | `bloom.vector-artwork`, schema 1, category Sources, output `image` |
| `artwork` | the data block holding the SVG (`bloom.vector-artwork.artwork`, integer id, 0 = none). Shown read-only on the card and in Properties as the file's name and size held, or "The SVG is missing from the project" |
| `fit` | contain-and-centre into the composition (`bloom.vector-artwork.fit`, default on); off keeps the file's own pixels, centred |
| Lowering | `NodeLoweringKind::VectorArtwork`: an expansion, not a pixel operation. The GPU coverage contract classifies it as non-pixel with the `scope.expansion` reason, like a node group: it renders through the lowerings its expansion builds |

The node is not offered in the Add menu or node search; a generic `bloom.node.add` of it makes an
empty node, which draws nothing.

### The SVG data block

The file's bytes are an opaque data block ([Data blocks](data-blocks.md)): type
`bloom.vector-artwork.svg`, schema 1.0, media type `image/svg+xml`, provenance the file's locator
(relative to the project folder when it is inside it; the `bloom.svg-import` producer when no
locator can be made) and its SHA-256. One block is held per digest: importing the same bytes again
reuses it. A block a Vector Artwork node names cannot be removed (project validation refuses the
missing reference), and a block no node names is kept until removed. A file that does not fit the
128 MiB the project holds for data blocks is refused by name, suggesting editable shapes. The
Assets record of the source file is recorded as for editable shapes; with the file gone it shows
missing and nothing else changes.

### Choosing the mode, and Break Apart (#642)

**Argument.** `bloom.layer.add-svg` takes `mode` (`layer` when omitted; anything else is an
`invalid-argument` naming both spellings). The registry factory (`createAddSvg`,
`src/scripting/operation_registry.cpp`) reads, hashes and parses the file with `prepareSvgSource()`
(and enumerates fonts when the file has text) where the operation is *created*: the client thread of
MCP, Python or the CLI, stopped by the call's `OperationContext::stop`. Applying it reads nothing
(`AddSvgArtwork(composition, PreparedSvgSource, ...)`), so a live MCP commit on the owner thread does
no file I/O and no font enumeration. A file that cannot be prepared still makes the operation, which
rejects with the reason when it applies (`withPreparationFailure`).

**Desktop.** `AssetController::chooseSvgImportMode(count, done)` is the one call every route shares
(File > Import, drops on Assets, Timeline and Viewer; later Add to Composition): it answers from
**Preferences > Import > SVG files** (`ApplicationPreferences::importSvgMode`, `import/svg-mode`:
`ask`, `layer`, `shapes`) when that is not Ask, otherwise opens `SvgImportModeDialog` over the active
window (As one layer selected, "Remember my choice" storing the answer). It asks once per batch;
Cancel imports none of the batch's SVG files. With no window to ask in (headless) the default, As one
layer, applies. The worker passes the mode to `AddSvgArtwork`.

**Break Apart.** `BreakApartSvgArtwork` (`bloom.layer.break-apart-svg`) finds the Vector Artwork node
that feeds the layer (`findLayerVectorArtwork`, through the nodes between), builds the description
from the block's bytes (`buildSvgArtwork`, fonts from the prepared source), writes it with ids from
the project (`SvgArtworkWriter`) and makes every reader of the node read the new root Merge, then
removes the node and its parameters. The holding layer is not touched, so its transform and keys,
parent, timing, masks, track matte, opacity, blend mode, the nodes between and the layers parented
to it stay as they are and apply to the root group as they apply to an editable import's. This is
exactly the expansion's resampled form (`composeLayerTransform` off), so the frame is identical
(`bloom.runtime.svg-artwork-layer`: before and after equal at keyed frames to the 2e-6 gate; a
scaled or turned layer equals the resampled expansion exactly and is the editable import's picture,
which a Merge resamples, so it is softer than the one layer's sharper route when scaled up). The
data block and the Assets record stay. `prepareVectorArtworkSource()` prepares the held copy where
file I/O is allowed: the desktop does it on the import worker, the registry in the operation factory.
One undo restores the layer exactly. The Composition menu and the Timeline layer context menu offer
it for a selected one-layer SVG.

### Expansion
### Expansion

`document::expandVectorArtwork(project, composition, context)` replaces every Vector Artwork node
of a composition with what the editable import builds from the block's bytes for that
composition's current format: the block is parsed (`parseVectorArtworkBlock`, bounded and
cancellable like a file import), described with `buildSvgArtwork` and written with ids derived from
the node's id (top bit set, never an id the project allocator hands out). Every edge that read the
node now reads the artwork's root Merge; a muted node is a muted root. The layer that held the node
keeps its opacity, blend mode, track matte, masks, parent, timing and any node between the source
and its slot, which apply to the artwork's root exactly as they apply to the root group of an
editable import. A node whose block is missing or does not parse stays unexpanded and is named in
`unresolved`. The context carries the installed fonts, an optional parsed-scene cache and a
cancellation callback; the import's warnings are recomputed by the expansion, not stored.

#### The layer's transform is drawn through the geometry

A Merge ends vector provenance ([Layer graph model](layer-graph-model.md)), so a layer that
transformed the artwork's root Merge would resample a picture drawn at the composition's scale:
soft when scaled up, turned or rendered past the composition's size. The expansion therefore moves
the holding layer's transform onto the geometry (`VectorArtworkComposedLayer`, on by default,
`composeLayerTransform`):

- a hidden **carrier** (a Layer node that draws nothing) takes the layer's position, anchor, scale
  and rotation parameters by their ids (so keys, drivers and a Viewer gesture's overrides move it),
  its parent and its frame pivot;
- every drawn layer of the artwork (each shape, text, gradient and matte source, and any group whose
  picture is filtered, such as a blurred group) is parented to the carrier, through the groups
  whose own matrix is the identity (the import pivots each group on its own centre that way);
- the holding layer draws at the identity; layers parented to it are parented to the carrier.

Each shape and text is then covered through the composed matrix at the output resolution, as any
parented shape layer is, on the CPU reference and the GPU alike; a gradient is still drawn in its
own window and resampled (it is a smooth ramp, and its matte's edge is the shape's). The layer
reports its own box: `CompiledLayerOutput::composedArtwork` names the carrier and the drawn layers,
and both routes publish the union of their content bounds through the carrier's matrix as the
layer's local box, polygon and anchor (`detail::publishComposedArtworkBounds`), so the Viewer's box,
handles and gestures see the layer as before.

Only a layer the composition cannot change is composed: the node feeds it directly and nothing else
(an entry layer, or a Layer node between the node and its slot), it pivots on the frame, it has no
masks (they are drawn in its content space) and identity timing (its content would run on another
clock than its transform). Any other layer is drawn at its output scale (below). Limits: a
stroke's round join is built in the shape's own units before the transform, so at a large scale
its arc is the same polygon scaled (a pixel or two of the join differ by a few coverage codes from
a file with the scale baked in); an anisotropic scale strokes with the transformed outline, where
the editable import of a stretched file gives the stroke one width and names the approximation.

#### A layer the composition changes is drawn at its output scale

A layer that pivots on its content, has masks, has its own timing or time remap, or reads the
artwork through nodes (a blur, a glow, a drop shadow, colour nodes) cannot take the artwork's
geometry into its own matrix: its masks, pivot, clock and effects all work on its content picture.
It is drawn as a scaled pre-composition would be instead (`VectorArtworkOversampledLayer`):

- a hidden **carrier** Layer node holds a constant scale `s` about the composition's origin (frame
  pivot, no parent) and parents every drawn layer of the artwork, as for a composed layer, so the
  shapes and text are covered `s` times larger;
- every length of the nodes between that is in composition pixels, and the layer's masks (paths,
  feather and expansion), are multiplied by `s` in the expanded copy: a constant times `s`, a keyed
  length as a new curve with its values and handle values times `s`, so the nodes see the picture
  a pre-composition `s` times larger would give them. The nodes allowed between are Gaussian Blur
  (radius, vertical radius), Box Blur (radius), Glow (size, length), Drop Shadow (distance,
  softness), Levels, Curves, Hue/Saturation, Grade, Colorize, Tone Map, Color Matrix, Alpha
  Association and Reroute; a muted node passes through;
- the layer keeps its own transform, pivot, masks, timing, parent, children, opacity, blend mode and
  track matte. The compiler sets `CompiledLayerOutput::contentScale = s`; the CPU reference and the
  GPU preparation both resample the content through `LayerMatrix::content` (the authored matrix with
  the pivot times `s` and the scale over `s`), so the one resample maps the larger picture back, and
  the layer's published local box is the picture's over `s` with the anchor where it was. Layers
  parented to it use its plain matrix.

**The band.** `s` is the smallest power of two at least the product of the largest |scale| the keys
reach (a held or linear segment's ends; an eased one's handles too; Back and Elastic overshoot widen
the span by their amplitude) over the layer and its parents, the Layer nodes that resample its
picture on the way to its Merge and their parents, and every group above it that is drawn larger
(below), and at least 2 when any of them turns. It is at most `kVectorArtworkMaxOversample` (64).
It is halved while a scaled length would leave its parameter's range (a blur radius past 256), and
while the estimate of its drawn window is past the pixel budget (below). `limited` says so when the
band is cut. A layer that neither scales past 1 nor turns, under groups that neither do, needs
nothing and is in none of `composed`, `oversampled` or `resampled`. A Viewer gesture's override of a
scale reaches the band (the context's `override`), and an override of a scaled length is multiplied
as the stored one is (`scaledParameters`).

**Only the window the frame shows is drawn.** The oversampled picture is not drawn whole: every
operation's window is clipped to what its readers can show of the frame (`detail::visibleSupportWindows`,
"What a layer draws" below), through the holding layer's matrix (the larger picture's pixels map to
the frame through it), its masks (they multiply their content pixel by pixel), the nodes between
(a transparent-edge blur widens the window by its taps, each axis by its own radius when the axes
are separate, a glow by its keys, octaves and streaks, a drop shadow by its move) and the groups
above. A layer scaled 8x in a 3840 x 2160 composition draws about one frame of pixels at 8x, not the
64 frames of the whole picture. The CPU reference and the GPU preparation take the same windows, so
the pixels are the same on both routes. The window is not clipped (the whole picture is drawn)
through a blur that repeats its picture's edge (Clamp), whose edge is its window's.

A **content-pivot layer** (#642 B4) turns and scales about its content's centre: the centre of the
bounds its input publishes, and an image effect (a mask, a blur) publishes its data window as its
bounds, so a window clipped to the frame would move the pivot with the frame. Above an oversampled
picture its pivot is instead taken from the bounds its input publishes with nothing clipped, worked
out for the frame before it is drawn, without a pixel (`detail::UnclippedPictures`,
src/runtime/visible_support.hpp, by the same rules and calls the CPU reference makes: a shape's
flattened path bounds and window, a solid's size, a Layer Output through its matrix, a Merge's
union, the masks, track mattes, blurs and colour kernels). Its window is then clipped like a frame
pivot's, and the pivot is exactly where it is when the whole picture is drawn, so a pan neither
moves it nor compiles anything. The CPU reference and the GPU preparation take the same pivot. Where
those bounds are not known before the frame is drawn (the artwork has text, whose bounds are its
rasterized words; a Glow or a Drop Shadow between, whose pictures' bounds follow a Transform 2D and a
directional pass; anything else the rules do not cover), the expansion budgets the whole picture and
the runtime draws it whole, as before. A content-pivot layer that is not above an oversampled
picture keeps drawing its input whole.

Over a mask or a blur, the content pivot is the centre of that kernel's data window, which carries
its input's window margins in the input's own pixels: the oversampled picture's margins are pixels of
the larger picture, so its pivot can sit a pixel or two of that picture (a fraction of a composition
pixel) from the pivot of a pre-composition baked `s` times larger (1.5 of the 4x picture's pixels in
`contentPivotDrawsItsWindow`). That is the runtime's rule for every image effect's bounds and
predates B4; it is named here, not changed.

**The pixel budget.** The band is halved while its window over the whole animation is estimated
past `kVectorArtworkMaxOversampledPixels` (8192 x 4096 full-resolution pixels, 512 MiB of float
RGBA). The estimate is made when compiling, for every frame at once: the artwork's area at a scale
of 1 (the fitted frame, or the file's viewport when not fitted) times `s`², and, where the window is
clipped, at most the frame's preimage through the smallest scale the layers above reach (a turn
widens it, at worst to (width + height)² / 2), each side widened by the nodes' reach. A whole-frame
artwork keyed from 1x to 8x is drawn at 2x (at its first key the frame shows the whole picture);
one scaled 8x is drawn at 8x. The limit names the budget: "drawn at 2x: its window at 4x is
estimated at ... pixels over the animation, past the pixel budget of one oversampled picture
(8192 x 4096)". Past the band's own cap: "drawn at 64x, the most any layer oversamples".

**Groups above the layer.** A Merge ends vector provenance: the group layer (or Layer node) that
resamples a group's picture would resample the artwork's pixels too. Each group above the holding
layer whose readers scale or turn is drawn at its own output scale instead
(`VectorArtworkOversampledGroup`, by its Merge): its band is that of the layers resampling it and
every group above it; every entry's layer of the Merge (one is made at its defaults, the identity,
for an entry that has none) draws its picture that many times larger into the group
(`CompiledLayerOutput::outputScale`, the layer's matrix followed by the scale about the origin;
its children use the plain matrix), the lengths of the nodes between an entry's Layer node and its
slot and between the Merge and its reader are multiplied, and the group's reader maps the picture
back with a content scale. The artwork inside is then drawn at the product: an icon in a group
scaled 3x is drawn at 4x, the group at 4x. A layer drawn into an oversampled group publishes the
bounds it would have at 1 (`publishOutputScaledBounds`, run by the CPU reference and the GPU
preparation alike), so the Viewer's boxes are where they were. A sibling raster in the group is
resampled twice (into the larger group and back), as a scaled pre-composition would. It is left so:
drawing it once would mean compositing it into the group's picture at a different scale from the
artwork it blends with (a Merge composites every entry in one pixel grid), or moving it out of the
group, which changes what it blends with; neither is a small change (#642 B4 looked). Named when it
cannot be (`groupsResampled`, by the Merge; the artwork is then drawn at the scale up to that group,
and the group's reader resamples its picture): an Adjustment entry (its nodes would apply to the
group's picture at the composition's scale), a Layer node shared with another Merge entry, a layer
matted by a node's Mask or by a layer outside the group, a node after an entry's Layer node that is
not known to draw the same at a larger scale, or a reader's scale driven by the graph.

**Cache identity.** The band and the group's scale are part of the plan. Every frame of an animated
layer renders from one plan and one oversampled picture of the artwork (the carrier's scale is
constant, so the artwork's operations keep their cache identity from frame to frame); a key edit
that leaves the band where it was is a new revision with the same oversampled picture; only an edit
that moves the band draws a different one. The window is not in the plan: it is resolved per frame
from the frame's transforms and is part of each frame's operation keys, so a pan compiles nothing
and a frame showing another window of the picture draws that window (the frame's cache entry, not
the plan's), while returning to a frame reads its kept pixels.

**Named when it cannot be sharp** (`resampled`, by the node's id, the layer then resampling the
artwork's picture at the composition's scale, as the editable import's root group does): a node
between that is not known to draw the same at a larger scale ("the `<type>` node between the
artwork and its layer is not known to draw the same at a larger scale"), one that reads another
image or has a scaled length driven by the graph, a layer or parent scale driven by the graph (its
largest scale is not known when compiling), a mask shaped by a node's Mask, more than one reader or
none, a Merge entry without a layer, the artwork read through another node, a muted artwork, or a
length that cannot be scaled at all.

Comparing with a file whose points are scaled by `s` (the scaled pre-composition): at a band of 2
the pixels are the same; at 4 and 8 the curves' flattenings differ (the one layer flattens in the
file's units through the band's matrix, the baked file at its own size, each within 1/32 pixel of
the curve), so where a curve passes that close to a pixel's 4 x 4 coverage samples some of the
sixteen samples may land on the other side: one or two along a slanted edge, up to one column or
row where the curve's tangent runs along the sample grid (a circle's leftmost point). The composed
route shows the same at 4x.

Cost (a suspicion from the code, not measured): the oversampled picture's drawn window is about the
frame's preimage times `s`², so its memory follows the output's size within the pixel budget, a
content-pivot layer's too; through text, a Glow or a Drop Shadow a content-pivot layer's window is
the whole picture, bounded only by the budget's estimate. Working out a content pivot's unclipped
bounds draws no pixel but flattens every shape's path of the picture once more per frame (the
frame's own drawing flattens them again), so a file of many shapes under a content pivot pays that
twice. The estimate takes the fitted frame or the file's viewport as the artwork's extent, not its
drawn bounds, so a small artwork in a large viewport is budgeted as the whole viewport.

#### Compiling a Vector Artwork layer

The snapshot compiler runs the expansion on every compile, after Node Groups and before anything is
resolved or lowered, so every route that reads a plan (the Viewer, RAM preview, still, sequence and
video export, headless and scripted render, nested compositions) sees ordinary nodes. Nothing here
is a pixel operation: the GPU coverage contract classifies `NodeLoweringKind::VectorArtwork` as
non-pixel (`scope.expansion`, like a Node Group), and every operation the expansion makes is an
ordinary, covered lowering. No exception or opt-out exists for it.

- **Parsed scenes** are kept between compiles by the block's SHA-256 and size
  (`runtime::VectorArtworkSceneCache`, `processVectorArtworkSceneCache()`): a new revision, another
  composition or a second layer of the same file reads the kept scene; a block whose recorded digest
  does not match its bytes is parsed every time and never kept. The cache is bounded: scenes count
  by an estimate of their elements and anchors against a 64 MiB budget, a scene larger than a quarter
  of it is parsed for the compile that asked and not kept, admitting one past the budget retires the
  least recently used first, and `retire()` drops them all. Parsing polls the compile's
  cancellation; a cancelled parse keeps nothing.
- **Fonts** are read from the process font resolver's installed-font index
  (`SystemFontIndex::snapshotBlocking`, on the compiling worker, never the UI thread) only when an
  artwork has text.
- **Plans** are cached per revision as for any composition (`CompiledPlanCache`). A keyed transform
  or opacity is a curve on the carrier or the layer, so every frame of an animated layer renders
  from one plan; editing the layer or the node is a new revision, compiled again from the kept scene.
- **Refusal.** A node whose block is missing, is not an SVG, is past 32 MiB or does not parse is
  refused by name: "Vector Artwork cannot be drawn", with the reason ("The SVG this layer holds is
  missing from the project", or the reader's diagnostic). A cancelled compile reports Cancelled.
- **Budget, admission and retirement** of the render are those of the operations the expansion
  makes: the CPU reference keeps the request's pixel budget, and the GPU phase admits, waits for or
  refuses the scene and returns every claimed byte when it is released.

Cost (a suspicion from the code, not measured): each compile rebuilds the artwork's description and
writes it into a copy of the composition, proportional to the file's elements, so a Viewer gesture
on a layer of a very large file (which compiles each override) repeats that work per update.
`bloom_runtime_gpu_svg_artwork_node_bench` measures the frame and the compile
([Benchmarks](../development/benchmarks.md)); it is written and has not been run.

#### Evidence

- `bloom.runtime.svg-artwork-layer` (CPU reference, through the compiler): the same parent,
  transform, keyed opacity, Screen blend, alpha track matte, mask and Gaussian Blur between source
  and slot on the root layer of either mode give the same whole frame (2e-6, frames 0 to 2); the
  plain layer equals the editable import (gradients, clip, mask, opacity, blur, dashes, text; fitted
  and not); a saved project reopened with the file deleted keeps the mode, the block's bytes and the
  frame; scaled 3x, scaled 2.2x and turned, and stretched and turned, the layer equals the editable
  import of the file with that transform baked into its points (fills exact; one join pixel of a
  stroke within a few codes) with as many edge pixels, where the resampled picture has three to
  seven times as many; its bounds carry its scale and rotation; one plan per revision, one parse per
  file, an override of the layer's position moves the artwork; cancellation, the scene cache's
  budget, admission and retirement, the refusal of an unreadable SVG, which layers resample and why,
  and a layer parented to the artwork's layer landing exactly where it did. Drawn at the output
  scale (`sharpUnderEveryLayerEdit`, `sharpOnAContentPivot`): masked and turned, retimed, with a
  blur and a drop shadow between, with a colour node between, masked under a scaled parent, and on
  a content pivot, each equals its scaled pre-composition (exact at a band of 2, up to two coverage
  samples on curved edges at 4) where the resampled picture differs on ten or more times the pixels
  with far more edge pixels; the content-pivot layer's bounds are the resampled layer's.
  `namedWhenItCannotBeSharp` names a vignette between, a driven blur radius and a driven scale,
  draws a masked layer scaled 6x at 4K at 8x, caps 100x at 64x, names the pixel budget for a
  whole-frame artwork keyed from 1x to 8x at 4K (2x), draws a whole-frame content pivot scaled 6x at
  4K at 8x (B4) and, with a Drop Shadow between (its whole picture drawn), resamples it with the
  pixel budget named, halves a blur radius of 100 to 2x, and leaves a 0.8x layer alone.
  `contentPivotDrawsItsWindow` (B4): a content-pivot Layer node scaled 3x (4x) and turned, panned so
  about a third of the picture leaves the frame, equals its scaled pre-composition up to two coverage
  samples on curved edges where the resampled picture differs on far more pixels, plain and masked
  with a blur between (the pre-composition's pivot matched, above); its box and pivot are the same
  panned as with all of it shown, where they equal its input's published bounds exactly, while its
  input is drawn in a smaller window; fitted and scaled 5x (8x), a 64 MiB request below the whole 8x
  picture (127 MB) renders the frame, and the same request is refused with a Drop Shadow between; a
  pan compiles once, a frame showing another window draws it and a frame shown again hits; a
  cancelled evaluation publishes nothing. `oversampledCacheIdentity`: one
  plan and cache hits over the frames of a keyed layer, a key in the same band hits, one past it
  draws at 8x, a scale override reaches the band and a mask path override is multiplied,
  cancellation and the pixel budget. Groups (`sharpInsideScaledGroups`, `groupBoundsAndSiblings`,
  `groupsNamedWhenTheyCannotBe`, `windowAndGroupIdentity`): the artwork in a group scaled 3x (group
  and artwork at 4x), in one scaled 1.6x and turned (2x) and in a group scaled 1.5x inside one scaled
  2x (4x and 2x) each equals its scaled pre-composition as above where the resampled picture
  differs on ten or more times the pixels; a sibling in the larger group publishes its bounds at 1
  exactly and the layers above within their edge pixels; a vignette between a group and its layer,
  a driven group scale and an Adjustment entry are named and the artwork is still drawn sharp
  through its own layer; a pan of a masked layer scaled 4x compiles once, a frame showing another
  window draws it and a frame shown again hits its pictures; a group keyed from 1.5x to 3x renders
  every frame from one plan in the 4x band and a key at 6x moves it to 8x; a cancelled compile and
  evaluation of a grouped artwork, and a request below its budget refused with the budget named.
- `bloom.runtime.gpu_svg_artwork_layer` (production GPU route): 512 x 288 over frames 0 to 2 with
  the layer's position, scale, rotation and opacity keyed, cold and warm, with the CPU's bounds
  (box, polygon and anchor) and at half resolution; the one layer on the device equals the editable
  import's CPU frame on the in-repo files; 1920 x 1080 and 3840 x 2160 prepare and run on the device;
  drawn at the output scale (masked with a blur and a drop shadow between, keyed scale, rotation and
  opacity, frames 0 to 2; a content pivot; retimed), cold and warm with the CPU's bounds, and at
  1920 x 1080 and 3840 x 2160 with a blur between (2x); in a group keyed from 2x to 3x and turned,
  masked, with a sibling (4x, frames 0 to 2), and scaled 5x with a blur, a drop shadow and a glow
  between, masked and panned (8x, frames 0 to 2, the largest window drawn about a sixth of the
  whole picture), cold and warm with the CPU's bounds; at 3840 x 2160, masked and scaled 4x and 8x
  and in a group scaled 3x, on the device only: the largest window drawn is one frame (4x, 8x) or
  16/9 of one (the group at 4x) where the whole picture would be 112 to 448 million pixels, and a
  384 x 216 crop around the circle's edge equals the scaled pre-composition on the device (exact at
  4x and in the group; at 8x 18 of 138 edge pixels differ, by at most one column of coverage
  samples) where the resampled picture differs on 850 to 2,200 pixels. A content-pivot Layer node
  (B4) with a blur between, masked, keyed from 1x to 3x (4x), turned and panned matches the CPU
  reference cold and warm with its bounds, 512 x 288 over frames 0 to 2, its box the same at every
  frame and the largest window at the panned frame about a third of the whole 4x picture; at
  3840 x 2160 one scaled 4x and 8x and turned 12 degrees draws at most one frame of pixels where the
  whole picture would be 112 and 448 million, and a crop around the circle's edge equals the scaled
  pre-composition on the device (exact at 4x; at 8x 54 of 347 edge pixels differ, by at most 0.17)
  where the resampled picture differs on 1,400 to 2,200 pixels. Phase admission, refusal and
  retirement, a cancelled preparation and run, and the executor's request budget.
  `--require-device` fails without a device.
- `bloom.ui.viewer_export_svg_artwork` runs every route for the one layer too (Viewer frames from the
  expected route, CPU route equal to GPU route, RAM preview cold and warm, still, range and movie
  with their provenance), and on the GPU route exports 1920 x 1080 and 3840 x 2160 stills from the
  device with no CPU reference frame, of the plain layer and of one with a blur between (drawn at its
  output scale), and at 3840 x 2160 of one masked and scaled 8x (drawn at 8x), of one in a group
  scaled 3x (drawn at 4x) and of one read by a content-pivot Layer node scaled 6x and turned (drawn
  at 8x, B4), each one device frame and no CPU reference frame. It found that the Viewer's and the export's scene caches
  together filled the process's image slots once every frame of the keyed layer drew new images;
  the caches now share one bound ([GPU backend](gpu-backend.md), "The scene cache keeps stacks").

The desktop import and the scripting `bloom.layer.add-svg` still pass `shapes`: they do not offer
the mode yet.

Reading a file for import (`prepareSvgSource`) is one shared, cancellable step: it reads at most
32 MiB, hashes and parses the file and enumerates installed fonts only when it has text, reporting
its phase (reading, finding fonts). A `PreparedSvgSource` holds the path, scene, bytes, digest,
fonts and named warnings, so applying the operation reads nothing.

## Structure

As editable shapes, the operation builds, inside the target composition:

| SVG | Bloom |
| --- | --- |
| the file | a Merge entry (the root group) named after the file |
| `<g>`, `<svg>`, `<a>`, `<switch>`, `<use>` with a name or opacity, clip or mask | a nested Merge entry that keeps the file's `id` or editor label and the painting order |
| a drawn element (`path`, `rect`, `circle`, `ellipse`, `line`, `polyline`, `polygon`) | a Shape layer whose `path` parameter is driven by its own **Path node** (`bloom.value-path`) |
| solid fill and solid stroke | the Shape's own fill and stroke |
| gradient fill | a Linear or Radial Gradient layer matted by the outline filled opaque (Alpha track matte) |
| gradient stroke, dashes | a Path Stroke node (dash length, gap, offset, cap, join) matted or drawn as a stroke outline |
| `clip-path`, `mask` | the clip or mask's content as a sibling entry used as a track matte (Alpha for clips, Luma for masks) |
| `<text>` | a native Text layer (position, size, colour, alignment, rotation). The file's `font-family`, weight and style go through the platform font catalogue (installed faces, System source); a family that is not installed, a generic family, or a weight or slope the family lacks is substituted and listed by name |
| `opacity` | the entry layer's opacity |
| `feGaussianBlur` (a filter of that one primitive on SourceGraphic) and CSS `filter: blur()` | a native Blur node (`bloom.gaussian-blur`) after the element, radius 3 sigma in composition pixels, clamped to 256; an uneven sigma is applied evenly and noted. Any other filter primitive is not applied and is listed |
| the file itself | an Assets record (kind Image) holding the file's project-relative locator and SHA-256, recorded once per path and digest; a missing or moved file is not an error |

Every shape and group is pivoted at the centre of its own bounds (Frame pivot with an explicit
Anchor and Position at that centre, snapped to 1/16 px so the matrix stays exactly the identity), so
rotating or scaling one turns it about itself with nothing moved on screen. Skipped and approximated
items are named by `id` or label, otherwise by position (`path 2 in 'art'`, `circle 1 in g 2 in the
file's root`).

Every element is therefore ordinary document data: its Path node can be keyed as a whole value
(see below), Trim Path, Offset Path, Boolean Paths, Merge Paths, Round Corners and Path Stroke take
the Path node's output, and MCP, Python and the CLI read and edit it with the operations they
already have. Names come from the file (`id`, `inkscape:label`, `data-name`), nodes are laid out in
a grid per group and framed by group in the node editor, and groups of more than 256 siblings are
held as consecutive runs, each a nested Merge, so no stack, card or render pass holds thousands of
entries at once. A plain composite of runs is the same picture.

### Baked transforms

Element and group `transform` attributes, the `viewBox` and the fit into the composition are
applied to the geometry on import. Groups carry opacity, clip and mask only. Rationale: a baked
point is exactly what an artist edits and keys, and the SVG transform stack has no counterpart that
would stay exact under editing. The fit is contain-and-centre by default (`fit=false` keeps the
file's own pixels).

### Animating points

Points animate through the Path node: a whole-value key interpolates point by point when both keys
hold the same contours and anchor counts, and morphs when they do not (different anchor counts,
contour counts or closed flags; path-value.md, "Morphing between different shapes", #597). The
Viewer's anchor and handle tools edit any contour of an imported shape, compound shapes included:
a press on a point of another contour makes it the contour under edit, a gesture edits one contour
and writes the whole value back with the other contours, the fill rule and the groups untouched,
and an edit at a time other than a key's keys the value there. A frame-pivot entry layer is never
compensated (its pivot does not follow the content). Contours are never dropped.

`bloom.runtime.gpu_svg_artwork_native` proves it on a B with two counters and an O: a corner of the
outline and a corner of the upper counter are dragged and keyed from frame 0; frame 0 equals the
import, the moved corner and counter change, the other counter and the O do not, and the GPU frame
equals the CPU reference. The same test wires Offset Path, Round Corners, Boolean Paths, Merge Paths
and Path Stroke to a keyed compound ring and reads each result. An earlier hand proof that showed a B
without counters had keyed Trim Path to 35 percent on the same path as well as moving both counters
30 px; Trim Path keeps the first 35 percent of the total outline length, which is the outer contour,
so the counters were trimmed away. Without the Trim they survive.

The Assets record of the source file is a source, not footage: the Assets panel shows it as present
when the file is on disk and missing, like moved footage, when it is not (it is never probed as an
image).

MCP, Python and the CLI key the same value: `bloom.animation.set-keyframe-at-time-for-parameter`
(and `bloom.animation.set-keys`) take a whole path or gradient object as the value, so a script can
move points of any contour of an imported shape at any time, on the Path node's `value` parameter.

## Reader

`src/media/vector` (`bloom_media_vector`, Qt-free) reads a file into an `SvgScene`: elements in
painting order with final coordinates, paints, gradients, clip and mask content, and a list of what
it did not take. It is Bloom's own bounded reader. Comparison made when choosing it: nanosvg
(small, no CSS, no clip/mask, no nested groups), lunasvg and resvg (full renderers, whose
scene-to-raster design is the opposite of what is wanted here: the product is editable layers, so
the reader must expose geometry, names and the group hierarchy, which a rasteriser does not), and
Qt SVG (Qt types are barred below `src/ui`). The comparison was by the libraries' documented
capabilities; no third-party code is vendored, so no dependency record is added.

Supported: the basic shapes and every path command (arcs become cubic Beziers), `transform`,
`viewBox` and `preserveAspectRatio`, nested `<svg>`, `<use>` and `<symbol>`, `<defs>`, the CSS
cascade (`<style>` with type, class, id, attribute, descendant and child selectors, the `style`
attribute, inheritance), `currentColor`, linear and radial gradients with `href` inheritance,
`gradientTransform`, `userSpaceOnUse` and `objectBoundingBox` units and all three spread methods,
clip paths and masks (bounding-box units on shapes), opacity, fill and stroke opacity, fill rule,
dashes, caps, joins and text with `tspan` positions and `text-anchor`.

Reported, never silently dropped (each item is named in the operation's warnings, in
the Jobs panel for the desktop import, and in the script result): a radial gradient's focal point
(drawn centred), patterns, filters other than a single Gaussian blur, markers, embedded or linked images, `foreignObject`, scripts,
animation, text-on-path, text stroke and gradient fills of text, per-span colours inside one text
run, multi-length dash patterns beyond the first dash and gap, anisotropic stroke widths, CSS the
reader does not apply, and anything hidden by `display: none`.

### Security and bounds

No external resource is ever fetched and nothing executes: external `href`s, external entities and
parameter entities are refused, scripts are listed and skipped. The XML reader is bounded: depth
128, 600,000 nodes, entity values that contain references are refused (no billion-laughs growth),
`use` cycles and expansion are guarded, at most 20,000 drawn elements, 3,000,000 anchors and
32 MiB of file. A contour of more than 4,096 anchors is skipped by name (Bloom's per-contour limit).
Reading polls a cancellation callback; the desktop reads the file on the blocking-I/O worker with
progress and the Jobs panel diagnostics, and applies the parsed scene on the document thread in one
transaction.

## Colour

SVG blends in encoded sRGB; Bloom composites in scene-linear light. Colours are converted at import
and gradient stops are resampled so the encoded-space ramp is followed, but translucent overlays
and alpha ramps composite in linear light and look lighter than a gamma-space blend. This is a
property of Bloom's colour model, not of the importer.

## Rendering and memory

Nothing here adds an image-producing lowering: an imported file renders through the Shape, Path,
Gradient, Text, Merge and track-matte lowerings already covered by the GPU coverage contract, at any
render scale. The import still warns when a file has more than 2,000 shapes.

On the GPU a frame holds a few images however many layers the file has. The executor folds each
Merge entry into the stack as it is drawn and frees it, a shape layer holds its fill, stroke and
composite only while it runs, and the phase claims that live set rather than one image per layer.
Coverage masks, command storage and the scene cache are counted as what they are: see
[GPU Backend, Scenes of many layers](gpu-backend.md#scenes-of-many-layers-343). Before this, a
4,000-element file was refused at 800 by 800, worldmap fell to the CPU on the command ceiling at
1920 by 1080, and tiger, bike and worldmap waited for device memory at 3840 by 2160 until the export
stopped.

### What a layer draws

A layer draws only what can be seen, on both routes alike. Its window is its transformed support
intersected with what its readers can show of the frame: the Composition Output asks for the frame,
a Merge and the pass-through kernels (identity, combine, pointwise and colour transforms) pass that
on, a frame-pivot layer maps it back through its own matrix, and a transparent-edge blur widens it
by its taps. A layer's masks, the grades, keys and channel moves (Levels, Curves, Hue/Saturation,
Grade, Colorize, Color Tool, Luma Key, Channel Select and Combine, Highlight Roll-Off, Alpha
Association) read each input only where they are read; a Transform 2D that only moves (a Drop
Shadow's offset) shifts the window by its move and widens it by its resample's taps; a directional
(Glow Streak) pass widens it by its four taps' reach. A content-pivot layer above an oversampled
Vector Artwork picture maps it back through its matrix too, its pivot taken from the bounds its input
publishes with nothing clipped ("Only the window the frame shows is drawn" above). Anything else
(any other content-pivot layer's input, a field image, any other kernel) asks for everything, so nothing it reads is clipped. A layer whose track matte is a plain alpha or luma
multiply is also clipped to the matte's window, since nothing outside it survives the matte
(`detail::visibleSupportWindows` and `detail::matteBoundedLayers`, src/runtime/visible_support.hpp,
used by the CPU reference's layer arm and by `CpuGpuSceneBuilder`). Before, an imported elliptical
gradient's layer took its whole transformed support, about ten frames for firefox, and the Merges
over it took the same window, so firefox held about 42 frames.

A gradient node itself still draws in its own window: the GPU gradient kernel needs that window's
origin at zero, so it is not sub-windowed. It is the import's elliptical reach on each axis (no
longer a square of the larger), and admission counts it at that size. Once the scene is prepared
the phase raises its claim to the executor's live set with every image at its prepared size
(`GpuProcessingPhaseLease::raiseExecutorNeed`); a ceiling below that refuses the scene instead of
drawing past it. The raise adds only the live set's excess over the executor bytes the admitted
need already counts. Once phases of an envelope have been measured, their need is the measured peak
plus a quarter rather than the stated working allowance (which phases do not fill), and a raise
keeps that: it never puts the unused working allowance back into the claim. Images are counted at what the device allocates, not at the requested extent: the allocator
rounds each one up (a 1920-wide float image allocates 2048 wide). The device reports its allocation
sizes (`GpuAllocationSizes`, probed with `vkGetImageMemoryRequirements` and memoised per size), the
phase budget rounds each claimed image with them, and the per-operation metadata buffers (axis
tables, staging, status words) are added to the claim. A test renders a scene with the executor's
request budget equal to that claim.

### Results

Results on the production headless export route (`bloom-cli run`, PNG still, the export requiring a
GPU device, one frame each). The memory columns come from the corpus mode of
`bloom.runtime.gpu_svg_artwork_native` (the same builder and executor). The phase claims the larger
of the admission floor and the live set at real image sizes. The last column is the same export on
the CPU reference route with no GPU device:

| File | Size | Route result | PNG | Commands | Live peak, serial / batched (frames) | Admission floor / live set at real sizes (frames) | CPU-only export |
|---|---|---|---|---|---|---|---|
| tiger (482 layers) | 1920x1080 | GPU | 216,093 B | 854 | 1.76 / 4.69 | 5 / 2.55 | |
| tiger | 3840x2160 | GPU | 524,084 B | 854 | 1.66 / 2.11 | 5 / 2.55 | identical PNG |
| bike (626 layers) | 1920x1080 | GPU | 205,099 B | 1,027 | 1.88 / 4.20 | 9 / 1.62 | |
| bike | 3840x2160 | GPU | 501,295 B | 1,027 | 1.74 / 1.79 | 9 / 1.61 | identical PNG |
| firefox | 1920x1080 | GPU | 233,629 B | 76 | 2.76 / 5.02 | 5 / 2.57 | |
| firefox | 3840x2160 | GPU | 543,192 B | 76 | 2.61 / 2.76 | 5 / 2.56 | within one code |
| worldmap | 1920x1080 | GPU | 141,463 B | 7,301 | 2.28 / 4.87 | 7 / 3.00 | |
| worldmap | 3840x2160 | GPU | 369,562 B | 7,301 | 2.27 / 2.27 | 7 / 2.99 | identical PNG |
| gen-dense-4000 | 1920x1080 | GPU | 1,030,099 B | 12,965 | 5.33 / 7.47 | 7 / 5.00 | |
| gen-dense-4000 | 3840x2160 | GPU | 2,927,949 B | 12,962 | 5.04 / 5.04 | 7 / 5.00 | within one code |

The interactive Viewer shows firefox at 1920 by 1080 and 3840 by 2160 from the GPU-prepared packed
product with no CPU fallback, on the application's own memory wiring
(`bloom_ui_viewer_export_svg_artwork_test --svg <file> --size <W>x<H> --loader <loader>`).

CPU/GPU parity is proven at 512 by 288 on every corpus file, whole frame, at 2e-6. The CPU column is
what the CPU reference holds for the whole frame:

| File | Commands | Live peak, serial / batched (frames) | Admission floor / live set at real sizes (frames) | CPU reference holds (frames) | GPU against CPU reference |
|---|---|---|---|---|---|
| tiger | 854 | 2.25 / 7.33 | 5 / 2.55 | 4 | exact |
| bike | 1,027 | 2.04 / 4.75 | 9 / 1.67 | 8 | exact |
| firefox | 76 | 19.5 / 21.1 | 5 / 18.5 | 4 | within 1.8e-7 |
| worldmap | 7,301 | 2.46 / 5.45 | 7 / 3.04 | 5 | exact |
| gen-dense-4000 | 12,982 | 2.50 / 5.60 | 7 / 2.27 | 5 | within 6.0e-8 |

Firefox's gradient nodes are drawn in the file's own units, so at 512 by 288 one is about 18 frames;
at 1920 by 1080 the same image is 1.3 frames. Admission counts it at that size. The live image peak
is measured with batching off; batched submission keeps up to eight kernels' outputs until their
fence, within its own byte bound.

### The CPU reference

The CPU reference folds Merges entry by entry as the executor does (`detail::mergeFoldSteps`,
src/runtime/merge_fold_order.hpp). An entry's own operations run just before it is composited,
bottom entry first, and the entry is released as soon as it is folded into the Merge's accumulator.
The accumulator grows by half again as entries reach past it, within what the frame shows of the
Merge; the pixels it adds are transparent, which every blend mode leaves where neither image has
coverage, so the image is the one plan order computes. Anything read from more than one place keeps
its plan position. The evaluator folds when plan order would hold more than eight images, or more
than the request allows, and folding holds less; smaller plans keep plan order. The preflight and
the evaluation take the same order, so what is admitted is what is held. Without a GPU device tiger,
bike, worldmap, firefox and the 4,000-element file now export at 3840 by 2160, and their PNGs equal
the GPU's (byte-identical for tiger, bike and worldmap; within one 8-bit code for firefox and the
4,000-element file).

## Re-adding an imported SVG (#646)

Every add-to-composition route for an SVG asset (`isSvgSourceAsset`, src/document) builds the
artwork through `addSvgAssetArtwork` / `AddSvgAssetNodes` (src/commands/svg_import_operations.cpp)
with the #642 mode, reusing the asset record. `prepareSvgAssetSource` reads the file when it is there
and its digest matches, else uses the project's held copy, else refuses by name. Routes prepare on the
asset controller's import worker (desktop) or on the client thread in the operation factory (script
routes, `mode` default `layer`); `AddImageLayer` and `AddImageNode` refuse an SVG asset. An empty image
layer made from an SVG asset by an older build is reported by `svgFootageWarning` /
`layerSvgFootageWarning` (src/document) on the card, timeline row, Properties and the MCP/Python
queries.
