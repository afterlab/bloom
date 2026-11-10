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

### Expansion

`document::expandVectorArtwork(project, composition, context)` replaces every Vector Artwork node
of a composition with what the editable import builds from the block's bytes for that
composition's current format: the block is parsed (`parseVectorArtworkBlock`, bounded and
cancellable like a file import), described with `buildSvgArtwork` and written with ids derived from
the node's id (top bit set, never an id the project allocator hands out). Every edge that read the
node now reads the artwork's root Merge; a muted node is a muted root. The layer that held the node
is untouched, so its transform, opacity, blend mode, track matte, masks, parent, timing and any node
between the source and its slot apply to the artwork's root exactly as they apply to the root group
of an editable import. A node whose block is missing or does not parse stays unexpanded and is
named in `unresolved`. The context carries the installed fonts, an optional parsed-scene cache and
a cancellation callback; the import's warnings are recomputed by the expansion, not stored.

`bloom.runtime.svg-artwork-layer` proves the contract on the CPU reference: the same parent,
transform, keyed opacity, Screen blend, alpha track matte, mask and Gaussian Blur between source
and slot on the root layer of either mode give the same whole frame (2e-6, frames 0 to 2); the
plain layer equals the editable import (gradients, clip, mask, opacity, blur, dashes, text; fitted
and not); and a saved project reopened with the file deleted keeps the mode, the block's bytes and
the frame.

The snapshot compiler does not expand the node yet: a composition holding one is refused with "The
SVG this layer holds was not expanded into its shapes." until the compiler runs the expansion.
The desktop import still makes editable shapes, since it offers no choice of mode yet.

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
by its taps. Anything else (a content-pivot layer's input, a field image, any other kernel) asks for
everything, so nothing it reads is clipped. A layer whose track matte is a plain alpha or luma
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
