# Node Catalogue

Status: working

Updated: 2026-11-15

## Purpose

This catalogue lists the nodes Bloom has shipped, the nodes it plans, and the families the product
map covers. It is a product map. Shipped nodes are listed by family with their current display
names; planned nodes are listed separately and each is tracked by an open issue. A node is shipped
only if it is registered in the code, so the two lists never overlap. Current priorities and
grouping of the planned work are tracked in backlog issue [#384](https://github.com/afterlab/bloom/issues/384).

Bloom has one canonical composition graph. The Nodes editor exposes that graph directly, while the
Timeline, Properties, and Viewer expose structured layer-friendly projections of the same nodes and
parameters. A feature available through a layer control and a node uses the same document identity,
primitive semantics, runtime operation, and render path.

The code is the source of truth for what is shipped. Display names are those of the Add menu and
the node cards; the stored type ID of a node never changes with its display name. The
[row-by-row reconciliation](node-catalogue-reconciliation.md) maps the historical research names
onto this catalogue.

## Delivery Levels

| Level | Meaning |
| --- | --- |
| `F0` | Foundation already represented in the document or runtime |
| `F1` | Rendering foundation required before the broad effect catalogue |
| `C1` | Essential compositing vocabulary |
| `M1` | Motion-design vocabulary built after the compositor is sound |
| `V1` | Advanced VFX, analysis, or temporally expensive work |
| `X1` | Optional pipeline domain; designed for modular contribution rather than core scope |

Levels express dependency and product importance, not a rigid release number. Nodes may enter
earlier when required by an end-to-end workflow, but they must not bypass the contracts of their
level's dependencies. They appear in the planned lists below; a shipped node has no level.

## Unified Scope And Status

This catalogue incorporates the historical Node Implementation Matrix as retained product research.
The [row-by-row reconciliation](node-catalogue-reconciliation.md) preserves all 219 historical rows
across 22 families and maps each to a shipped node, a planned node tracked by an open issue, or
no plan. The shipped and planned lists in this document and that reconciliation together define the
unified scope; omission from a short list here does not retire a reconciliation entry.

The two sources contribute different things:

| Source | Contribution | Limitation resolved here |
| --- | --- | --- |
| Historical implementation matrix | Broad 2D vocabulary, fields, points, animation modifiers, production utilities, and implementation-source research | Alias-heavy; mixes nodes, tools, recipes and services; library choices were not adopted and predate the current C++ architecture |
| Repository product catalogue | Delivery levels, one graph, typed domains, color/alpha/time contracts, CPU reference behavior and optional pipelines | Abbreviated several historical families |

Keep three questions separate: **what capability is intended**, **how it is exposed**, and **what
is implemented**. An entry can become a node, an operation selector, a preset/recipe, an editor
tool, or an explicit task. Combining implementation primitives does not require removing useful
artist-facing controls. Conversely, alternate names do not require duplicate durable node types.

A planned entry is not a commitment to a date or a final contract: its node contract, standalone UI
and delivery level are decided when it is built. Existing architecture contracts take precedence.
Implementation status comes from the registered definitions, evaluators and tests; neither a
delivery level nor the reconciliation is a full implementation audit. The exact implemented value
vocabulary is owned by [`value-node-library.md`](../architecture/value-node-library.md).

## Unified Family Overview

The table shows the shared scope of each family and where it stands today. Detailed historical
names and their individual treatment remain in the reconciliation.

| Family | Unified scope | Where it stands |
| --- | --- | --- |
| Sources and generators | Solid, image/sequence/video/composition, text, shape, gradients, diagnostic patterns | Shipped: Image, Video, Audio, Composition, Solid, Checker, three gradients, Test Card, Text Source, Shape, Vector Artwork. Planned: a unified Gradient node ([#551](https://github.com/afterlab/bloom/issues/551)) |
| Graph structure and reuse | Layer/Composition Output, Layer Stack, groups, reroutes, switches, exposed controls, nested compositions | Shipped: Layer, Merge, Output, Node Group, Group Input, Group Output, Reroute, typed value switches, Composition source. Planned: Image Switch ([#531](https://github.com/afterlab/bloom/issues/531)) |
| Compositing and alpha | Merge, mix, matte, premultiply, alpha replacement | Shipped: Merge, Image Mix, Set Alpha, Multiply Alpha, Alpha Association, Edge Extend. Planned: Transition / Wipe Matte ([#520](https://github.com/afterlab/bloom/issues/520)) |
| Channels and production passes | Split/combine/copy/shuffle/remove, pass extraction, Cryptomatte | Shipped: the channel nodes, Pass, Cryptomatte and per-pixel Image Math, Map Range and Clamp. Deep images and AOV reconstruction recipes are not planned |
| Color and grading | OCIO, LUTs, exposure, levels, curves, white balance, tone mapping | Shipped: Color Space Transform, Apply LUT, Grade, Levels, Curves, Hue/Saturation, Exposure, Offset, Multiply, Invert, Colorize, Auto Exposure, Tone Map, Color Matrix |
| Transform and distortion | Transform, resize, crop, fit, perspective, ST Map, displacement, lens and warp | Shipped: Transform 2D, Crop, Pad, Fit, Resize, Flip, Rotate 90, Tile & Mirror, Camera Shake, Corner Pin, ST Map, Displace, Lens Distortion, Rolling Shutter, Chromatic Aberration, Procedural Warp. Planned: 2D rig and puppet deformation ([#606](https://github.com/afterlab/bloom/issues/606)) |
| Filters and light | Blur, sharpen, glow/shadow, denoise, defocus | Shipped: the blur family, Sharpen, Denoise, Glow, Light Wrap, shadows, Lens Flare, Light Rays, Depth Fog, Z Defocus |
| Keying and mattes | Luma/chroma/difference keys, despill, cleanup, path masks | Shipped: Luma Key, HSL Qualifier, Mask, Mask Combine, Image to Mask, Matte Blur, Matte Erode, Matte Dilate. Planned: Channel Key, Chroma Key, Despill ([#485](https://github.com/afterlab/bloom/issues/485)), Matte Refine ([#515](https://github.com/afterlab/bloom/issues/515)) |
| Image time and motion | Offset, hold, remap, echo/trails, motion blur, flow, feedback | Shipped: Echo, Frame Rate Hold, Motion Blur, Vector Motion Blur, Optical Flow, Optical Flow Motion Blur. Planned: Time Offset, Frame Hold, Time Remap ([#346](https://github.com/afterlab/bloom/issues/346)), Flow Retime ([#522](https://github.com/afterlab/bloom/issues/522)) |
| Values, strings and data | Scalar/vector/color/Boolean math, conversions, strings, time/readouts, curve/ramp/table/point-set readers | Shipped: the full typed value library, including Number List. Planned: per-item list math ([#628](https://github.com/afterlab/bloom/issues/628)) and list conversions ([#629](https://github.com/afterlab/bloom/issues/629)) |
| Animation modifiers | Oscillator, wiggle, loop value, snap, audio-driven values | Shipped: Oscillator, Wiggle, Loop Value, Ping-pong, Wrap, Snap, Light Flicker, Audio Amplitude and the other audio analysis nodes. Bake to keyframes is a command, not a node: it writes a value node's Scalar output over the composition as keyframes. Planned: Simulation ([#594](https://github.com/afterlab/bloom/issues/594)) for stateful motion |
| Vector shapes | Primitive paths, fill/stroke, trim/offset/boolean/repeat, SVG | Shipped: Shape, Path value, Transform Path, Trim Path, Offset Path, Boolean Paths, Merge Paths, Round Corners, Path Stroke, Resample Path, Warp Path, Warp Path (Function), Shape Morph, Mask Path, Function Plot, Axes, SVG import. Planned: labeled axes from coordinate spaces ([#596](https://github.com/afterlab/bloom/issues/596), [#591](https://github.com/afterlab/bloom/issues/591)) |
| Text | Text source, outlines, layout, on-path text, animators | Shipped: Text Source, Text Outline. Planned: shaped layout, Text Animator, Text On Path ([#345](https://github.com/afterlab/bloom/issues/345)), Text To Shape / Mask ([#529](https://github.com/afterlab/bloom/issues/529)), keyed term groups ([#592](https://github.com/afterlab/bloom/issues/592)), Math Source ([#593](https://github.com/afterlab/bloom/issues/593)) |
| Fields and points | Typed spatial fields; point sets, scatter, cloning | Shipped: sixteen Fields nodes. Planned: point generators, Clone To Points and Draw Instances ([#526](https://github.com/afterlab/bloom/issues/526)) |
| Procedural design | Noise, Voronoi, patterns | Shipped through Fields: Noise, Voronoi, Pattern, Random Field and Shape Distance. Particles are planned ([#521](https://github.com/afterlab/bloom/issues/521)) |
| Tracking, analysis and inspection | Track/solve/stabilize, statistics, scopes, comparison | Shipped: Stabilize with a point tracker, Histogram, Image Statistics, Image Compare, Layer Bounds, Image Preview and a Scopes panel. Planned: planar tracking and camera solve ([#513](https://github.com/afterlab/bloom/issues/513)), measured bounds before the image pass ([#601](https://github.com/afterlab/bloom/issues/601)) |
| Audio | Decode, waveform/spectrum/amplitude analysis, effects | Shipped: Audio source and five effects, plus eight analysis nodes. A general realtime audio graph is not planned |
| Output, caching and extensions | Export, preview, caches, Python tasks, add-ons | Shipped: Output, Image Preview, the export queue, Python and MCP automation. Planned: File Cache as part of [#521](https://github.com/afterlab/bloom/issues/521). OpenFX hosting, a custom kernel boundary and an expression runtime are not planned |
| Optional 3D domains | Geometry, scene/camera/light, materials, particles | Planned: 3D camera and 3D layer space ([#605](https://github.com/afterlab/bloom/issues/605)), particles ([#521](https://github.com/afterlab/bloom/issues/521)); the rest are optional candidates with no open issue |

## Delivery Through Complete Workflows

Use the levels as dependency guides, with these workflow gates:

1. Prove reliable sources, transforms, image/color contracts and output, including save/reopen and
   deterministic CPU rendering.
2. Complete a practical compositor: composite, grade, blur, extract/clean mattes, manipulate
   channels and reuse groups. Advanced pass formats wait for their image/channel contracts.
3. Build motion-design workflows from values, curves, text and shape geometry, procedural design,
   audio analysis and bounded stateless fields/points.
4. Add expensive temporal processing, tracking, reconstruction and simulation only with explicit
   dependency windows, cancellation, cache invalidation and random-access/seek behavior.
5. Introduce optional domains when a concrete workflow needs their coherent module contributions.

This sequence guides future work; it does not undo features already implemented out of order.
Expression execution, public native kernels and external plug-in hosting require separate adopted
contracts. Choices made in the historical research that do not match the repository's C++20
architecture were not adopted. Follow the owning [`GPU`](../architecture/gpu-backend.md),
[`scripting`](../architecture/scripting-and-addons.md), and
[`module`](../architecture/module-system.md) documents for those decisions.

Current priorities and grouping of the unfinished work are tracked in backlog issue
[#384](https://github.com/afterlab/bloom/issues/384).

## Shipped Nodes


Bloom ships 259 node types, grouped as the Add menu groups them. Each row gives the current display name, the stored type ID and what the node does. Planned nodes are not in these tables; see [Planned Nodes](#planned-nodes). Layer boundaries show the layer's own name on the card; "Layer" is the node type.

| Family | Nodes |
| --- | --- |
| Sources | 15 |
| Compositing | 8 |
| Channels | 10 |
| Keying & Mattes | 9 |
| Transform | 10 |
| Distort | 7 |
| Blur & Sharpen | 12 |
| Light & Shadow | 8 |
| Stylize | 6 |
| Color Correction | 12 |
| Color Management | 2 |
| Time | 2 |
| Text & Paths | 19 |
| Tracking & Analysis | 11 |
| Fields | 16 |
| Values | 17 |
| Math | 18 |
| Convert | 20 |
| String | 15 |
| Logic | 11 |
| Time Values | 7 |
| Color Values | 7 |
| Vector | 7 |
| Utilities | 8 |
| Output | 2 |

### Sources

Image producers, plus the five audio effects, which sit beside the Audio source they follow. Audio nodes carry an Audio stream rather than an image.

| Display name | Type ID | Purpose |
| --- | --- | --- |
| Image | `bloom.image-source` | Reads a still image or image sequence asset. |
| Video | `bloom.video-source` | Decodes a video asset with exact frame timing and preserved alpha. |
| Audio | `bloom.audio-source` | Plays an audio asset as an audio stream. |
| Composition | `bloom.composition-source` | Nests another composition as an image and audio source with explicit time mapping. |
| Solid | `bloom.solid-source` | Fills the composition with one color. |
| Checker | `bloom.checker-source` | Generates a two-color checkerboard. |
| Linear Gradient | `bloom.linear-gradient` | Generates a linear color gradient with animated endpoints. |
| Radial Gradient | `bloom.radial-gradient` | Generates a radial color gradient. |
| Four-Color Gradient | `bloom.four-color-gradient` | Blends four colors anchored at four points. |
| Test Card | `bloom.test-card` | Draws a diagnostic pattern (bars, grid, ramps, zone plate, alpha gradient) from a formula at render size. |
| Audio Equalizer | `bloom.audio-equalizer` | Filters the frequency balance of an audio stream. |
| Audio Compressor | `bloom.audio-compressor` | Reduces the dynamic range of an audio stream. |
| Audio Auto Level | `bloom.audio-auto-level` | Automatically levels the loudness of an audio stream. |
| Audio Mono | `bloom.audio-mono` | Folds a stereo audio stream down to mono. |
| Audio Hiss | `bloom.audio-hiss` | Adds seeded noise (hiss) to an audio stream. |

### Compositing

Layer-level compositing and alpha. Merge is the one layer-stack operator; Layer and Output boundaries are listed here and under Output.

| Display name | Type ID | Purpose |
| --- | --- | --- |
| Merge | `bloom.layer-stack` | Merge: composites the stable, ordered layer inputs into one image and one audio mix. |
| Adjustment | `bloom.adjustment` | Applies the effects wired into it to everything composited below it. |
| Layer | `bloom.layer-output` | The boundary that makes a graph result a layer addressable on the Timeline, with transform, timing and blend values. |
| Image Mix | `bloom.image-mix` | Interpolates two images by an animated unit factor. |
| Set Alpha | `bloom.set-alpha` | Replaces alpha with a channel read from a second image. |
| Multiply Alpha | `bloom.multiply-alpha` | Multiplies alpha by a channel read from a second image. |
| Alpha Association | `bloom.alpha-association` | Converts between premultiplied and straight alpha with defined zero-alpha behavior. |
| Edge Extend | `bloom.edge-extend` | Reconstructs RGB around transparent edges to remove dark or bright fringes. |

### Channels

Channel and per-pixel math nodes. Channels keep their roles; data, masks, depth and IDs are not color.

| Display name | Type ID | Purpose |
| --- | --- | --- |
| Channel Copy | `bloom.channel-copy` | Copies channels from a source image into a destination image. |
| Pass | `bloom.channel-pass` | Extracts one named layer of a multi-channel image as a flat image. |
| Select Channels | `bloom.channel-remove` | Keeps or drops channels by pattern. |
| Shuffle | `bloom.channel-shuffle` | Rearranges channels by role. |
| Separate Channels | `bloom.separate-channels` | Splits an image into red, green, blue and alpha images. |
| Combine Channels | `bloom.combine-channels` | Builds an image from separate channel inputs. |
| Image to Channel Set | `bloom.image-to-channel-set` | Adds an ordinary image to an image as a named channel set. |
| Image Math | `bloom.image-math` | Per-pixel arithmetic between images and values, with selectable operation. |
| Image Map Range | `bloom.image-map-range` | Remaps an image's values from one range to another. |
| Image Clamp | `bloom.image-clamp` | Limits an image's values to a range. |

### Keying & Mattes

Matte extraction and cleanup. Mattes are Mask values, which connect to mask inputs and never to image sockets.

| Display name | Type ID | Purpose |
| --- | --- | --- |
| Luma Key | `bloom.luma-key` | Keys an image into a soft mask by Rec. 709 luma thresholds. |
| Cryptomatte | `bloom.cryptomatte` | Selects objects from Cryptomatte ID channels as a mask, and optionally as a matted image. |
| Mask | `bloom.mask` | A mask node holding one path with feather and expansion; layer masks use the same parameter set. |
| Mask Combine | `bloom.mask-combine` | Adds, subtracts, intersects or takes the difference of two masks. |
| Image to Mask | `bloom.image-to-mask` | Turns an image alpha, luma or channel into a mask. |
| Matte Blur | `bloom.matte-blur` | Gaussian blur of a mask. |
| Matte Dilate | `bloom.matte-dilate` | Grows a mask (maximum over a neighborhood). |
| Matte Erode | `bloom.matte-erode` | Shrinks a mask (minimum over a neighborhood). |
| HSL Qualifier | `bloom.hsl-qualifier` | Selects pixels whose hue, saturation and luma fall in chosen ranges, as a soft mask. |

### Transform

Geometric operations on an image.

| Display name | Type ID | Purpose |
| --- | --- | --- |
| Transform 2D | `bloom.transform-2d` | Position, anchor, scale and rotation with defined sampling and transparent borders. |
| Camera Shake | `bloom.camera-shake` | Seeded handheld, blast or gate-weave shake applied as a transform with motion blur. |
| Stabilize | `bloom.stabilize` | Tracks one feature with a cancellable point tracker and holds the picture still (Stabilize) or carries it along (Match Move). |
| Crop | `bloom.crop` | Trims the data and display windows by four animated pixel margins. |
| Pad | `bloom.pad` | Grows the data and display windows by four animated pixel margins. |
| Fit | `bloom.fit` | Fits or fills an image into an explicit output extent, centered. |
| Resize | `bloom.resize` | Resamples an image to an explicit output extent and pixel aspect. |
| Flip | `bloom.flip` | Exact horizontal or vertical flip. |
| Rotate 90 | `bloom.rotate-90` | Exact clockwise quarter turns. |
| Tile & Mirror | `bloom.tile-mirror` | Tiles, scrolls, mirrors or folds the picture into a kaleidoscope. |

### Distort

Lens, shutter and coordinate-map warps.

| Display name | Type ID | Purpose |
| --- | --- | --- |
| Lens Distortion | `bloom.lens-distortion` | Removes or adds radial and tangential lens distortion; also outputs the matching ST Map. |
| Chromatic Aberration | `bloom.chromatic-aberration` | Separates red and blue radially to imitate a lens color fringe. |
| Rolling Shutter | `bloom.rolling-shutter` | Skews and wobbles the picture to imitate a row-by-row sensor readout. |
| Corner Pin | `bloom.corner-pin` | Pins the picture to four corner points with perspective. |
| ST Map | `bloom.st-map` | Warps an image by absolute coordinates read from another image. |
| Displace | `bloom.displace` | Warps an image by relative offsets read from another image. |
| Procedural Warp | `bloom.procedural-warp` | Formula-driven warps: twirl, bulge, pinch, ripple, wave, polar wrap, loupe, bend, turbulence and raindrops. |

### Blur & Sharpen

Blurs, motion blur, flow, sharpening and denoising. The blur contract is in [`image-filters.md`](../architecture/image-filters.md).

| Display name | Type ID | Purpose |
| --- | --- | --- |
| Gaussian Blur | `bloom.gaussian-blur` | Separable Gaussian blur with optional separate axes and per-channel radius. |
| Box Blur | `bloom.box-blur` | Box blur on the shared separable neighborhood path. |
| Z Defocus | `bloom.z-defocus` | Blurs by per-pixel distance read from a depth pass. |
| Motion Blur | `bloom.motion-blur` | Transform-derived motion blur over a declared shutter interval. |
| Vector Motion Blur | `bloom.vector-motion-blur` | Motion blur driven by a motion-vector image. |
| Optical Flow | `bloom.optical-flow` | Estimates per-pixel motion between frames as a vector image. |
| Optical Flow Motion Blur | `bloom.optical-flow-motion-blur` | Motion blur driven by flow estimated from the image itself. |
| Directional Blur | `bloom.directional-blur` | Blur along one direction. |
| Spin Blur | `bloom.spin-blur` | Rotational blur around a center. |
| Zoom Blur | `bloom.zoom-blur` | Radial zoom blur from a center. |
| Sharpen | `bloom.sharpen` | Unsharp-mask sharpening. |
| Denoise | `bloom.denoise` | Denoise and Smooth: median, bilateral, dust and scratches, and temporal modes. |

### Light & Shadow

Light, glow and shadow effects, several reading a depth pass or the picture's alpha shape. The Add menu also offers a Glowing paths preset that builds a stroke-and-glow chain.

| Display name | Type ID | Purpose |
| --- | --- | --- |
| Glow | `bloom.glow` | Light-domain glow with threshold and compositing modes. |
| Light Wrap | `bloom.light-wrap` | Wraps background light around a foreground edge. |
| Drop Shadow | `bloom.drop-shadow` | Matte, blur, offset and composite recipe for a shadow behind the picture. |
| Inner Shadow | `bloom.inner-shadow` | Darkens the inside of the picture's alpha shape along its edges. |
| Inner Glow | `bloom.inner-glow` | Lights the inside of the picture's alpha shape from its edges or middle. |
| Depth Fog | `bloom.depth-fog` | Thickens fog with distance read from a depth pass. |
| Lens Flare | `bloom.lens-flare` | Adds a stack of lens-scatter elements around a bright source. |
| Light Rays | `bloom.light-rays` | Adds volumetric rays radiating from a point. |

### Stylize

Finishing looks.

| Display name | Type ID | Purpose |
| --- | --- | --- |
| Vignette | `bloom.vignette` | Darkens or tints the frame toward its edges. |
| Film Grain | `bloom.film-grain` | Adds seeded, repeatable film grain. |
| Codec Degrade | `bloom.codec-degrade` | Imitates heavily compressed video: blocking, color bleed and keyframe refresh. |
| Alpha Contour | `bloom.alpha-contour` | Draws an outline along the picture's alpha edge. |
| Quantize | `bloom.quantize` | Threshold to black and white, or posterize to a few levels per channel. |
| Dither | `bloom.dither` | Reduces levels per channel with even noise so gradients break into grain instead of bands. |

### Color Correction

Image grades that never convert color; each keeps HDR and negative RGB unless its purpose says otherwise.

| Display name | Type ID | Purpose |
| --- | --- | --- |
| Grade | `bloom.grade` | Lift, gamma, gain, offset, exposure, temperature and tint grading in the ASC CDL form. |
| Levels | `bloom.levels` | Black and white points, gamma and optional clamp. |
| Curves | `bloom.curves` | Versioned curves on RGB, channel, luminance and hue/saturation/luma axes. |
| Hue/Saturation | `bloom.hue-saturation` | Hue, saturation and lightness adjustment. |
| Exposure | `bloom.exposure` | Scales scene-linear RGB by 2^stops; alpha preserved. |
| Offset | `bloom.offset` | Adds an amount to scene-linear RGB; no implicit clamp. |
| Multiply | `bloom.multiply` | Scales scene-linear RGB by a factor; no implicit clamp. |
| Invert | `bloom.invert` | Maps straight scene-linear RGB to 1 - RGB; alpha preserved. |
| Colorize | `bloom.colorize` | Fill, Tint, Tritone, Gradient Map or Black and White color mapping. |
| Auto Exposure | `bloom.auto-exposure` | Follows the footage's brightness and neutralizes its color over time. |
| Tone Map | `bloom.tone-map` | Compresses highlights, adjusts shadows and optionally pulls out-of-gamut colors back. |
| Color Matrix | `bloom.color-matrix` | Applies a 4 by 4 color matrix with declared domain and alpha behavior. |

### Color Management

The two OCIO-backed transforms. Their display names dropped OCIO in [#609](https://github.com/afterlab/bloom/issues/609) because neither needs a configuration chosen; the stored type ids keep it.

| Display name | Type ID | Purpose |
| --- | --- | --- |
| Color Space Transform | `bloom.ocio-colour-space-transform` | Color Space Transform: converts between two color spaces of the project's OCIO configuration. |
| Apply LUT | `bloom.ocio-file-transform` | Apply LUT: applies a LUT file with interpolation, direction and look bypass. |

### Time

Image time processing. Time Offset, Frame Hold and Time Remap are planned.

| Display name | Type ID | Purpose |
| --- | --- | --- |
| Echo | `bloom.echo` | Combines delayed copies of the input over a bounded temporal window; includes Posterize time. |
| Frame Rate Hold | `bloom.frame-rate-hold` | Shows its input at a lower frame rate by holding each picture. |

### Text & Paths

Text and the path domain. The Add menu's Rectangle, Ellipse, Polygon, Star, Line, Triangle and Path entries add a Shape with that preset. Vector Artwork is made by importing an SVG and is not offered in the Add menu.

| Display name | Type ID | Purpose |
| --- | --- | --- |
| Text Source | `bloom.text-source` | Authorable text source: content, size, color, font, alignment and box layout. |
| Shape | `bloom.shape-source` | One parametric or path shape source with shared fill and stroke; the Rectangle, Ellipse, Polygon, Star, Line, Triangle and Path entries add it as presets. |
| Vector Artwork | `bloom.vector-artwork` | Imported SVG artwork drawn from vector geometry at output resolution; created by SVG import, not the Add menu. |
| Function Plot | `bloom.function-plot` | Samples a function into open path contours. |
| Axes | `bloom.axes` | Two axes with tick marks as a path, and the tick counts. |
| Axis Tick | `bloom.axis-tick` | One tick of an axis as a number, position and label text, to label with a Text Source. |
| Transform Path | `bloom.transform-path` | Moves, rotates, scales and skews a path. |
| Trim Path | `bloom.trim-path` | Keeps a start-to-end portion of a path by length. |
| Offset Path | `bloom.offset-path` | Grows or shrinks a path outline. |
| Boolean Paths | `bloom.boolean-paths` | Union, subtract, intersect or exclude paths. |
| Merge Paths | `bloom.merge-paths` | Concatenates paths into one path value. |
| Round Corners | `bloom.round-corners` | Rounds the corners of a path. |
| Path Stroke | `bloom.path-stroke` | Turns a path into the outline a stroke covers, with taper, dashes and brush stamps. |
| Mask Path | `bloom.mask-path` | Reads layer or Mask node outlines as a path. |
| Text Outline | `bloom.text-outline` | Converts the glyph outlines of a Text Source to a path. |
| Resample Path | `bloom.resample-path` | Re-spaces or recounts the points of a path. |
| Warp Path | `bloom.warp-path` | Distorts a path (bend, twist, ripple, bulge, pinch and more). |
| Warp Path (Function) | `bloom.warp-path-function` | Distorts a path by a user-defined function. |
| Shape Morph | `bloom.shape-morph` | Interpolates between two paths. |

### Tracking & Analysis

Image and audio analysis. Audio analysis nodes read an Audio input and give numbers or number lists; the [audio analysis guide](../user-guide/audio-analysis.md) has the controls.

| Display name | Type ID | Purpose |
| --- | --- | --- |
| Histogram | `bloom.histogram` | Measures the distribution of an image channel over a range and bins and draws it as an image. |
| Image Compare | `bloom.image-compare` | Difference, split or checkerboard comparison of two images. |
| Image Statistics | `bloom.image-statistics` | Reduces the pixels of an image to five scalars. |
| Audio Amplitude | `bloom.audio-amplitude` | The level of an audio stream over time as a number from 0 to 1. |
| Audio Waveform | `bloom.audio-waveform` | The audio waveform as a number list. |
| Audio Spectrum | `bloom.audio-spectrum` | The audio frequency spectrum as a number list. |
| Audio Bands | `bloom.audio-bands` | The level of a frequency range as one number from 0 to 1, with normalization, attack and release. |
| Audio Sample | `bloom.audio-sample` | Reads one value from an audio waveform or spectrum list. |
| Audio Features | `bloom.audio-features` | Reads onset, brightness, stereo width and similar features of the sound. |
| Audio Beat | `bloom.audio-beat` | Tempo, beat phase and beat pulse of the sound. |
| Audio Pitch | `bloom.audio-pitch` | Pitch class (chroma) and note of the sound. |

### Fields

Typed spatial functions, specified in [`fields.md`](../architecture/fields.md). A field has no resolution; generators also have image and mask outputs, and Rasterize Field and Field Sample are the explicit boundaries.

| Display name | Type ID | Purpose |
| --- | --- | --- |
| Combine Field | `bloom.field-combine` | Builds a vector or color field from lane fields. |
| Field Derivative | `bloom.field-derivative` | Gradient, curl or divergence of a field by central differences. |
| Force Field | `bloom.field-force` | Vortex, radial or uniform force as a vector field with falloff. |
| Image Field | `bloom.field-from-image` | Reads an image at the field position as scalar, vector or color field. |
| Gradient Field | `bloom.field-gradient` | Linear, radial, angular or diamond gradient as a field. |
| Field Math | `bloom.field-math` | Arithmetic, interpolation and vector operations between fields. |
| Noise | `bloom.field-noise` | Seeded Perlin, simplex or value noise with fractal sums, with gradient and curl outputs. |
| Pattern | `bloom.field-pattern` | Stripes, checker, rings, dots, grid lines, hex and wave patterns as a field. |
| Random Field | `bloom.field-random` | Per-cell seeded random scalar, vector or color field. |
| Rasterize Field | `bloom.field-rasterize` | Turns a field into an Image or Mask over an explicit extent. |
| Field Remap | `bloom.field-remap` | Remaps a field range, with clamp, invert, steps, bias, gain or a curve. |
| Field Sample | `bloom.field-sample` | Reads a field at one position as Scalar, Vector 2 and Color values. |
| Separate Field | `bloom.field-separate` | Splits a vector or color field into x, y, z and w lane fields. |
| Shape Distance | `bloom.field-shape-distance` | Signed distance and falloff to a primitive shape or a mask, as fields. |
| Field Transform | `bloom.field-transform` | Translates, rotates, scales, warps, tiles, mirrors or kaleidoscopes the domain of a field. |
| Voronoi | `bloom.field-voronoi` | Seeded Worley cells: distance, cell id, cell position and edge distance. |

### Values

Typed value sources and composition readouts. The value library is specified in [`value-node-library.md`](../architecture/value-node-library.md); Number List is specified in [`number-list.md`](../architecture/number-list.md).

| Display name | Type ID | Purpose |
| --- | --- | --- |
| Composition Duration | `bloom.composition-duration` | The composition duration in seconds. |
| Composition Size | `bloom.composition-size` | The composition's pixel size as a value. |
| Data Sample | `bloom.data-sample` | Samples a stored data curve at a position. |
| Point Set | `bloom.point-set-read` | Reads a count and one indexed point from a stored point set. |
| Ramp Sample | `bloom.ramp-sample` | Samples a color ramp at a position. |
| Table Lookup | `bloom.table-lookup` | Looks up a row of a stored table. |
| Boolean | `bloom.value-boolean` | A Boolean value. |
| Color | `bloom.value-color` | A color value. |
| Integer | `bloom.value-integer` | An integer value. |
| Number List | `bloom.value-number-list` | An ordered list of up to 65,536 finite numbers. |
| Path | `bloom.value-path` | One path value, keyable as a whole. |
| Scalar | `bloom.value-scalar` | A scalar value. |
| String | `bloom.value-string` | A string value. |
| Vector 2 | `bloom.value-vector2` | A two-component vector value. |
| Vector 3 | `bloom.value-vector3` | A three-component vector value. |
| Layer Bounds | `bloom.value.layer-bounds` | Layer Bounds: the evaluated size, origin, anchor and center of an image, read after the image pass. |
| Light Flicker | `bloom.light-flicker` | A seeded flickering brightness number for an unsteady light. |

### Math

Checked scalar and integer arithmetic, mapping, shaping and motion utilities. Math and Integer Math carry operation selectors, so one node covers many operations.

| Display name | Type ID | Purpose |
| --- | --- | --- |
| Clamp | `bloom.clamp` | Limits a scalar to a range. |
| Degrees To Radians | `bloom.degrees-to-radians` | Converts degrees to radians. |
| Function | `bloom.function` | Evaluates a typed formula of its inputs. |
| Integer Math | `bloom.integer-math` | Saturating integer arithmetic. |
| Loop Value | `bloom.loop-value` | Repeats a value over a period. |
| Map Range | `bloom.map-range` | Remaps a scalar from one range to another. |
| Math | `bloom.math` | Scalar arithmetic, trigonometry, power and rounding with a selectable operation. |
| Mix | `bloom.mix` | Interpolates between two scalars. |
| Oscillator | `bloom.oscillator` | Sine, square, triangle or saw value of time. |
| Ping-pong | `bloom.ping-pong` | Bounces a value back and forth within a range. |
| Radians To Degrees | `bloom.radians-to-degrees` | Converts radians to degrees. |
| Random | `bloom.random` | Seeded random scalar. |
| Rounding | `bloom.rounding` | Floor, ceiling, round, truncate and fraction. |
| Sign | `bloom.sign` | Sign of a scalar. |
| Smoothstep | `bloom.smoothstep` | Smooth interpolation between two edges. |
| Snap | `bloom.snap` | Snaps a scalar to a step. |
| Wiggle | `bloom.wiggle` | Seeded smooth random motion over exact time. |
| Wrap | `bloom.wrap` | Wraps a scalar into a range. |

### Convert

Explicit conversions between value kinds; nothing converts implicitly.

| Display name | Type ID | Purpose |
| --- | --- | --- |
| Boolean To Integer | `bloom.boolean-to-integer` | Converts a Boolean to an Integer. |
| Boolean To Scalar | `bloom.boolean-to-scalar` | Converts a Boolean to a Scalar. |
| Boolean To String | `bloom.boolean-to-string` | Converts a Boolean to a String. |
| Color To String | `bloom.color-to-string` | Writes a Color as text. |
| Color To Vector 3 | `bloom.color-to-vector3` | Reads a Color's RGB as a Vector 3. |
| Integer To Boolean | `bloom.integer-to-boolean` | Converts an Integer to a Boolean. |
| Integer To Scalar | `bloom.integer-to-scalar` | Converts an Integer to a Scalar. |
| Integer To String | `bloom.integer-to-string` | Writes an Integer as text. |
| Scalar To Boolean | `bloom.scalar-to-boolean` | Converts a Scalar to a Boolean. |
| Scalar To Integer | `bloom.scalar-to-integer` | Converts a Scalar to an Integer. |
| Scalar To String | `bloom.scalar-to-string` | Writes a Scalar as text. |
| String To Boolean | `bloom.string-to-boolean` | Parses text as a Boolean. |
| String To Color | `bloom.string-to-color` | Parses text as a Color. |
| String To Integer | `bloom.string-to-integer` | Parses text as an Integer. |
| String To Scalar | `bloom.string-to-scalar` | Parses text as a Scalar. |
| Vector 2 To String | `bloom.vector2-to-string` | Writes a Vector 2 as text. |
| Vector 2 To Vector 3 | `bloom.vector2-to-vector3` | Widens a Vector 2 to a Vector 3. |
| Vector 3 To Color | `bloom.vector3-to-color` | Reads a Vector 3 as a Color's RGB. |
| Vector 3 To String | `bloom.vector3-to-string` | Writes a Vector 3 as text. |
| Vector 3 To Vector 2 | `bloom.vector3-to-vector2` | Narrows a Vector 3 to a Vector 2. |

### String

String utilities. Text is measured in Unicode scalars and case mapping is ASCII-only.

| Display name | Type ID | Purpose |
| --- | --- | --- |
| Case | `bloom.string-case` | Changes the case of text (ASCII only). |
| Character At | `bloom.string-character-at` | Reads one character of text by index. |
| Concatenate | `bloom.string-concatenate` | Joins strings. |
| Contains | `bloom.string-contains` | Tests whether text contains a substring. |
| Ends With | `bloom.string-ends-with` | Tests whether text ends with a suffix. |
| String Equals | `bloom.string-equals` | Tests two strings for equality. |
| Format | `bloom.string-format` | Formats values into text with a pattern. |
| Length | `bloom.string-length` | The length of text in Unicode scalars. |
| Pad | `bloom.string-pad` | Pads text to a length. |
| Repeat | `bloom.string-repeat` | Repeats text. |
| Replace | `bloom.string-replace` | Replaces a substring. |
| Split | `bloom.string-split` | Splits text and reads one part. |
| Starts With | `bloom.string-starts-with` | Tests whether text starts with a prefix. |
| Substring | `bloom.string-substring` | Takes part of text. |
| Trim | `bloom.string-trim` | Trims whitespace from text. |

### Logic

Comparisons, Boolean logic and typed switches.

| Display name | Type ID | Purpose |
| --- | --- | --- |
| Boolean Logic | `bloom.boolean-logic` | And, or and exclusive or of Booleans. |
| Boolean Not | `bloom.boolean-not` | Inverts a Boolean. |
| In Range | `bloom.in-range` | Tests whether a Scalar lies within a range. |
| Compare | `bloom.compare` | Compares two scalars with an explicit tolerance. |
| Switch Boolean | `bloom.switch-boolean` | Selects one of two Booleans by a Boolean. |
| Switch Color | `bloom.switch-color` | Selects one of two Colors by a Boolean. |
| Switch Integer | `bloom.switch-integer` | Selects one of two Integers by a Boolean. |
| Switch Scalar | `bloom.switch-scalar` | Selects one of two Scalars by a Boolean. |
| Switch String | `bloom.switch-string` | Selects one of two Strings by a Boolean. |
| Switch Vector 2 | `bloom.switch-vector2` | Selects one of two Vector 2 values by a Boolean. |
| Switch Vector 3 | `bloom.switch-vector3` | Selects one of two Vector 3 values by a Boolean. |

### Time Values

Frame, rate and timecode values. Timecode is non-drop-frame.

| Display name | Type ID | Purpose |
| --- | --- | --- |
| Time | `bloom.value-time` | A Time value read as seconds, frame and timecode. |
| Frame Number | `bloom.frame-number` | The frame the request time falls in. |
| Frame Rate | `bloom.frame-rate` | The composition frame rate. |
| Seconds To Frames | `bloom.seconds-to-frames` | Converts seconds to frames. |
| Frames To Seconds | `bloom.frames-to-seconds` | Converts frames to seconds. |
| Seconds To Timecode | `bloom.seconds-to-timecode` | Writes seconds as non-drop-frame timecode. |
| Timecode To Seconds | `bloom.timecode-to-seconds` | Reads non-drop-frame timecode as seconds. |

### Color Values

Operations on Color values. The image Separate and Combine Channels nodes are the ones for images.

| Display name | Type ID | Purpose |
| --- | --- | --- |
| Separate HSV (Value) | `bloom.separate-hsv` | Splits a Color into hue, saturation and value. |
| Combine HSV (Value) | `bloom.combine-hsv` | Builds a Color from hue, saturation and value. |
| Hue Shift (Value) | `bloom.hue-shift` | Rotates the hue of a Color value. |
| Luminance (Value) | `bloom.luminance` | The luminance of a Color value. |
| Mix Color | `bloom.mix-color` | Interpolates between two Colors. |
| Separate Color (Value) | `bloom.separate-rgba` | Splits a Color value into red, green, blue and alpha. |
| Combine Color (Value) | `bloom.combine-rgba` | Builds a Color value from red, green, blue and alpha. |

### Vector

Vector 2 and Vector 3 operations.

| Display name | Type ID | Purpose |
| --- | --- | --- |
| Vector 2 Math | `bloom.vector2-math` | Arithmetic on two Vector 2 values. |
| Vector 3 Math | `bloom.vector3-math` | Arithmetic on two Vector 3 values. |
| Vector 2 Measure | `bloom.vector2-reduce` | Length, dot product, distance and similar measures of Vector 2 values. |
| Vector 3 Measure | `bloom.vector3-reduce` | Length, dot product, cross magnitude, distance and similar measures of Vector 3 values. |
| Rotate 2D | `bloom.rotate-2d` | Rotates a Vector 2 by an angle. |
| Polar To Cartesian | `bloom.polar-to-cartesian` | Converts a radius and angle to a Vector 2. |
| Cartesian To Polar | `bloom.cartesian-to-polar` | Converts a Vector 2 to a radius and angle. |

### Utilities

Graph plumbing and encapsulation. Frames (named, drawn groups of cards) are editor layout, not node types.

| Display name | Type ID | Purpose |
| --- | --- | --- |
| Reroute | `bloom.reroute` | Graph organization point with no evaluation behavior. |
| Separate XY | `bloom.separate-xy` | Splits a Vector 2 into X and Y. |
| Combine XY | `bloom.combine-xy` | Builds a Vector 2 from X and Y. |
| Separate XYZ | `bloom.separate-xyz` | Splits a Vector 3 into X, Y and Z. |
| Combine XYZ | `bloom.combine-xyz` | Builds a Vector 3 from X, Y and Z. |
| Group Input | `bloom.group-input` | The boundary inside a Node Group that exposes typed inputs. |
| Group Output | `bloom.group-output` | The boundary inside a Node Group that exposes typed outputs. |
| Node Group | `bloom.node-group` | An instance of a reusable group of nodes with typed exposed ports. |

### Output

Nodes that consume an image.

| Display name | Type ID | Purpose |
| --- | --- | --- |
| Output | `bloom.composition-output` | The single evaluation endpoint of a composition. |
| Image Preview | `bloom.image-preview` | Inspection endpoint that shows the image at any point in the graph and never changes project output. |

### Behavior Notes For Shipped Nodes

- Image Mix interpolates two images by an animated unit factor. Its base input owns the output
  window; the second input is aligned in composition space and is transparent outside its data
  window. Its mask input takes a Mask. Set Alpha reads a second image as coverage with an
  Alpha/Red/Green/Blue/Luma selector.
- Exposure scales scene-linear straight RGB by `2^stops`. Offset and Multiply act on scene-linear
  straight RGB with unchanged alpha and no implicit clamp, including for negative and HDR RGB.
  Invert maps straight RGB to `1 - RGB` and keeps alpha.
- Color nodes never treat RGBA as an unlabeled four-component vector. Each operation declares its
  required processing domain, preserves HDR and negative RGB unless its purpose says otherwise, and
  keeps display transforms out of scene/process pixels.
- Resize and Fit use an explicit output extent and pixel aspect with premultiplied bilinear
  resampling. Crop and Pad change data and display windows with exact pixel mapping. Flip and
  Rotate 90 are exact pixel permutations.
- Linear and Radial Gradient use animated source-pixel endpoints and scene-linear straight RGBA
  interpolation with independent alpha, Clamp/Repeat/Mirror and an explicit extent.
- Video decodes on a worker only. Native decode providers exist on Linux; native macOS and Windows
  providers are not available yet (see [`platform-support.md`](../architecture/platform-support.md)).
- OCIO-backed nodes report a typed unavailable fallback where OCIO execution is not supported on a
  platform rather than substituting another path.
- Gaussian Blur and Box Blur follow the [radius, edges, alpha and halo contract](../architecture/image-filters.md).
  Several other nodes are specified in the same document and in
  [`evaluation-primitives.md`](../architecture/evaluation-primitives.md).

## SVG Assets And Vector Sources

SVG import is shipped. An SVG file imports as native Bloom layers, not as a picture, in one of two
modes chosen once per import batch (Preferences > Import > SVG files remembers the choice). The
full contract is [`vector-artwork.md`](../architecture/vector-artwork.md); the artist's view is the
[vector artwork guide](../user-guide/vector-artwork.md).

| Mode | Result |
| --- | --- |
| As one layer (default) | One layer whose source is a Vector Artwork node (`bloom.vector-artwork`, category Sources, Image output). The project holds the file's bytes in an opaque data block, one block per SHA-256 digest, so the layer renders, saves and reopens with the original file gone. A file over the 128 MiB data-block limit is refused by name and the editable mode is suggested |
| As editable shapes | A Merge group per SVG group, a Shape layer and Path node per drawn element, a Linear or Radial Gradient with a matte for gradient fills, a native Text layer per text element, native Gaussian Blur for the one supported filter, and clip paths and masks as track mattes. Every element is ordinary document data: it can be keyed, driven and edited like any other |

Behavior that is shipped:

- Adding an SVG asset to a composition again reuses the asset record, reading the file when it is
  present and matches, else the project's held copy.
- A Vector Artwork layer stays sharp at any scale, rotation and render size. The layer's transform
  is drawn through the geometry when the composition cannot change it, and otherwise the artwork is
  drawn at a power-of-two output scale bounded by the pixel budget. Downstream raster effects end
  vector provenance, and raster images embedded in an SVG do not become vectors.
- Break Apart converts a one-layer SVG into the editable-shapes structure in one undoable step; the
  data block and the Assets record stay.
- The reader is Bloom's own bounded, cancellable, Qt-free reader. It supports the basic shapes and
  path commands, transforms, `viewBox`, nested documents, `use`, the CSS cascade, gradients with
  all spread methods, clip paths, masks, opacity, dashes, caps, joins and text with the file's
  fonts. Nothing executes and no external resource is fetched; depth, node, element, anchor and
  file size are bounded.
- Anything it does not take is named in the import warnings rather than silently dropped: patterns,
  filters other than one Gaussian blur, markers, embedded or linked images, `foreignObject`,
  scripts, animation, text on path and a few text-paint cases. Animated or scripted SVG and
  editable SVG export are not part of the shipped behavior.
- SVG blends in encoded sRGB and Bloom composites in scene-linear light, so translucent overlays
  and alpha ramps can look lighter than in a gamma-space blend; colors and gradient stops are
  converted at import.
- Imported SVG renders through the Shape, Path, Gradient, Text, Merge and track-matte operations
  already covered by the GPU coverage contract; it adds no new image-producing operation.

Vector sharpness through nested compositions and instances is tracked in [#607](https://github.com/afterlab/bloom/issues/607) and is not shipped.

## Planned Nodes

Every entry below has an open issue and is not shipped. Level is the dependency level from
[Delivery Levels](#delivery-levels); where one issue covers several nodes, the issue lists them.
This is not a priority order; see [#384](https://github.com/afterlab/bloom/issues/384).

### Compositing, Keying And Mattes

| Level | Planned node | Issue |
| --- | --- | --- |
| `C1` | Image Switch: selects one of several image inputs | [#531](https://github.com/afterlab/bloom/issues/531) |
| `C1` | Channel Key, Chroma Key, Despill | [#485](https://github.com/afterlab/bloom/issues/485) |
| `C1` | Matte Refine | [#515](https://github.com/afterlab/bloom/issues/515) |
| `C1` | Transition / Wipe Matte | [#520](https://github.com/afterlab/bloom/issues/520) |

### Time, Motion And Tracking

| Level | Planned node | Issue |
| --- | --- | --- |
| `M1` | Time Offset, Frame Hold, Time Remap | [#346](https://github.com/afterlab/bloom/issues/346) |
| `V1` | Flow Retime (optical-flow speed change) | [#522](https://github.com/afterlab/bloom/issues/522) |
| `V1` | Planar tracking, applying tracks to Corner Pin, and camera solve (the point tracker and Stabilize are shipped) | [#513](https://github.com/afterlab/bloom/issues/513) |
| `X1` | Simulation: a seekable stateful solve with values and events | [#594](https://github.com/afterlab/bloom/issues/594) |

### Text, Paths And Shapes

| Level | Planned node | Issue |
| --- | --- | --- |
| `M1` | Shaped Text Layout, Text Animator, Text On Path | [#345](https://github.com/afterlab/bloom/issues/345) |
| `M1` | Text To Shape and Text To Mask | [#529](https://github.com/afterlab/bloom/issues/529) |
| `M1` | Text as paths with keyed term groups | [#592](https://github.com/afterlab/bloom/issues/592) |
| `M1` | Math Source: typeset math as paths | [#593](https://github.com/afterlab/bloom/issues/593) |
| `M1` | Labeled axes driven by a coordinate space (Axes and Axis Tick are shipped without that link) | [#596](https://github.com/afterlab/bloom/issues/596) |
| `M1` | Coordinate spaces: named spaces readable as values | [#591](https://github.com/afterlab/bloom/issues/591) |
| `M1` | Measured bounds before the image pass, for placement | [#601](https://github.com/afterlab/bloom/issues/601) |

### Points, Instances And Particles

| Level | Planned node | Issue |
| --- | --- | --- |
| `M1` | Point generators (Point, Grid, Scatter, Points Along Path, Shape To Points, Text To Points), Clone To Points (the Instancer) and Draw Instances | [#526](https://github.com/afterlab/bloom/issues/526) |
| `X1` | Particle System, with File Cache | [#521](https://github.com/afterlab/bloom/issues/521) |

### Values And Lists

| Level | Planned node | Issue |
| --- | --- | --- |
| `C1` | Per-item number-list math | [#628](https://github.com/afterlab/bloom/issues/628) |
| `C1` | List to Path, List to Image and List to Points | [#629](https://github.com/afterlab/bloom/issues/629) |

### Sources, Deformation And Camera

| Level | Planned node | Issue |
| --- | --- | --- |
| `C1` | One unified Gradient node replacing Linear Gradient and Radial Gradient, with the color picker widget system | [#551](https://github.com/afterlab/bloom/issues/551) |
| `V1` | 2D rig and puppet deformation | [#606](https://github.com/afterlab/bloom/issues/606) |
| `X1` | 3D camera and 3D layer space | [#605](https://github.com/afterlab/bloom/issues/605) |

### Optional Pipeline Domains

These families validate Bloom's modular architecture but do not expand the core compositing scope.
They should arrive as coherent modules when a real workflow requires them. Only the items with an
issue above are planned; the rest are candidates.

| Level | Domain | Candidate node families |
| --- | --- | --- |
| `X1` | Geometry | Mesh/curve/point primitives, transform, selection fields, attributes, topology, instances, realization, sampling, and geometry output |
| `X1` | 3D Scene | Scene input, object/collection query, camera, light, transform hierarchy, render, depth, normal, motion, and ID outputs |
| `X1` | Materials | Typed material inputs, closures, texture sampling, coordinate transforms, and material output through adopted interchange contracts |
| `X1` | Particles | Emitters, forces, simulation zones, attributes, instancing, caching, and render conversion ([#521](https://github.com/afterlab/bloom/issues/521)) |
| `X1` | Audio | Decode, mix, filter, envelope, analysis, resample, and output nodes with realtime-safe execution classes; the source, five effects and eight analysis nodes are shipped |
| `X1` | Pipeline | Asset query, validation, publish, conversion, build, and external-process tasks with explicit trust and cancellation boundaries |

## Candidates Without An Open Issue

These product-map entries are neither shipped nor tracked by an open issue. They stay as ideas, not
plans; the reconciliation records how each historical name relates to them. A candidate becomes
planned when an issue is opened for it.

| Family | Candidates | Level |
| --- | --- | --- |
| Sources | Grid generator | `C1` |
| Transform | Align, Distribute, Auto-Orient | `M1` |
| Compositing | Holdout, Replace Alpha, Deep Merge, Deep Holdout, Deep To Image, Depth Composite | `C1`, `V1` |
| Color | Vibrance, Color Match | `C1`, `V1` |
| Filters | Edge Detect, color-image Dilate/Erode, Bokeh Blur, Convolution, Glare | `C1`, `V1` |
| Keying | Difference Key, Advanced Keyer, Edge Color | `C1`, `V1` |
| Time | Trails, Delay/Feedback | `M1`, `V1` |
| Analysis | Sample Image | `M1` |
| Paths | Vectorize Image, a path repeater and shape-along-path (partly within [#526](https://github.com/afterlab/bloom/issues/526)) | `V1`, `M1` |
| Values | Matrix and transform compose/decompose primitives | `C1` |

## Catalogue Rules

- A node has a stable namespaced type ID, schema version, typed ports, parameter roles, diagnostics,
  execution class, and lowering recipe.
- Similar UI does not justify duplicate math. Nodes compose shared checked primitives and domain
  kernels.
- Nodes declare whether they are uniform, pointwise, neighborhood, warp/gather, reduction, global,
  temporal, analysis, or side-effecting task operations. This drives tiling, region-of-interest,
  caching, scheduling, and GPU eligibility.
- Image ports carry semantic channel roles, color-encoding identity where applicable, alpha
  association, data/display windows, pixel aspect, and coordinate conventions. Data, masks, depth,
  normals, motion, UVs, and IDs are not color.
- Unsupported nodes remain preservable and diagnosable. Bloom never silently substitutes a
  different effect, quality level, color path, or alpha equation.
- CPU reference behavior defines correctness. GPU implementations declare supported semantics and
  prove parity within per-operation tolerances.
- Add-ons and optional modules register through the same node-definition and evaluator boundaries;
  they do not mutate the built-in catalogue or bypass commands, snapshots, tasks, and diagnostics.
