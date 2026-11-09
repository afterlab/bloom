# Vector Artwork (SVG import)

Status: canonical

Updated: 2026-11-09

Issue: [#343](https://github.com/kinetik-gg/bloom/issues/343).

An SVG file imports as native Bloom layers, not as a picture. An artist can select any element,
move its points, key them, fill it with another colour, run Trim Path or Offset Path over it, and
the render is the production evaluation path: preview, still export, sequence export and headless
render all read the same document. This document records the design, the supported profile, the
limits and what is reported rather than imported.

## Structure

`bloom.layer.add-svg` (`AddSvgArtwork`, `src/commands/svg_import_operations.cpp`) is one operation
and so one undo step. It builds, inside the target composition:

| SVG | Bloom |
| --- | --- |
| the file | a Merge entry (the root group) named after the file |
| `<g>`, `<svg>`, `<a>`, `<switch>`, `<use>` with a name or opacity, clip or mask | a nested Merge entry that keeps the file's `id` or editor label and the painting order |
| a drawn element (`path`, `rect`, `circle`, `ellipse`, `line`, `polyline`, `polygon`) | a Shape layer whose `path` parameter is driven by its own **Path node** (`bloom.value-path`) |
| solid fill and solid stroke | the Shape's own fill and stroke |
| gradient fill | a Linear or Radial Gradient layer matted by the outline filled opaque (Alpha track matte) |
| gradient stroke, dashes | a Path Stroke node (dash length, gap, offset, cap, join) matted or drawn as a stroke outline |
| `clip-path`, `mask` | the clip or mask's content as a sibling entry used as a track matte (Alpha for clips, Luma for masks) |
| `<text>` | a native Text layer (position, size, colour, alignment, rotation), set in Bloom's font |
| `opacity` | the entry layer's opacity |

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
hold the same contours and anchor counts (point-count morphing is #597 and is out of scope). The
Viewer's anchor and handle tools edit any contour of an imported shape, compound shapes included:
a press on a point of another contour makes it the contour under edit, a gesture edits one contour
and writes the whole value back with the other contours, the fill rule and the groups untouched,
and an edit at a time other than a key's keys the value there. A frame-pivot entry layer is never
compensated (its pivot does not follow the content). Contours are never dropped.

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
(drawn centred), patterns, filters, markers, embedded or linked images, `foreignObject`, scripts,
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

## Rendering and budget

Nothing here adds an image-producing lowering: an imported file renders through the Shape, Path,
Gradient, Text, Merge and track-matte lowerings already covered by the GPU coverage contract, at any
render scale. Every layer is a full-frame image when a frame is drawn, so very dense files cost
memory in proportion to the layer count; the import warns when a file has more than 2,000 shapes.
A 4,000-element file renders at a smaller frame, and at 800 by 800 the GPU admission refuses it
with a budget diagnostic rather than falling back silently.
