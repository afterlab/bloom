# Planned node delivery

Status: first 18 nodes implemented and locally checked; later chunks remain planned

Updated: 2026-11-04

Owner: [issue #331](https://github.com/kinetik-gg/bloom/issues/331).

## Delivered checkpoint

Exposure, Offset, Multiply, Invert, Linear Gradient, Radial Gradient, Checker, Image Mix, Set Alpha,
Multiply Alpha, Crop, Pad, Flip, Rotate 90, Resize, Fit, Gaussian Blur and Box Blur are implemented.
Six independent audit groups supplied fixes integrated before the final optimized application build.
Ten CPU/UI/persistence groups and five required-device native GPU groups passed at the node milestone;
the final session/audio integration group passes 21 cases at `79a81217`. Native execution was not
repeated after changes confined to audio, undo, recovery and test fixtures. This is bounded local
Linux evidence, not final-source qualification of every export route or Windows/macOS execution.

Standalone Transform 2D is implemented by #336 (see
[evaluation primitives](../architecture/evaluation-primitives.md#transform-2d)). Matte Blur is
implemented by #337 (see [image filters](../architecture/image-filters.md#matte-blur)). Luma Key,
Matte Erode and Matte Dilate are implemented by #338 (see
[image filters](../architecture/image-filters.md#luma-key)). Levels, Curves and Hue/Saturation are
implemented by #340 (see [image filters](../architecture/image-filters.md#levels-curves-and-huesaturation)).
Separate Channels and Combine Channels are implemented by #479 (see
[image filters](../architecture/image-filters.md#separate-channels-and-combine-channels)).
Drop Shadow, Glow and Light Wrap are implemented by #339 (see
[image filters](../architecture/image-filters.md#drop-shadow-glow-and-light-wrap)).
Chunks #341–#349 remain planned. The
inventory below records
the original audit baseline rather than claiming those delivered nodes remain absent.

## Verified inventory

At source baseline `45431368`, assembly registers 112 types: 12 image/audio/structural definitions,
34 foundational value/control types and 66 descriptor utilities. This is a source-derived count, not
an executed registry dump or a catalogue completion percentage. The product catalogue mixes node types,
selector operations, recipes, UI surfaces and optional domains.

| Area | Present | Remaining gap |
| --- | --- | --- |
| Sources | Solid, Text, Shape, Image/sequence, Video, Audio, Composition Source | Gradients, Checker/Grid/Bars and procedural image generators |
| Shape | Seven modes, fill/stroke, Viewer drawing and pen editing | Several shape operands are constant-only; path streams/modifiers absent |
| Text | Raster text, font/layout controls, editable Viewer text | Portable shaping, animators and text-on-path |
| Composition | Layer transforms, Layer Stack displayed as Merge, Composition Output, nested composition | Image Mix/matte/alpha operators, standalone transform/canvas nodes |
| Color effects | OCIO Color Space Transform and File Transform | Ordinary grading, filter, keying and matte effects |
| Values | Constants, math, vectors, switches, time, random, conversions and strings | Exposed advanced scalar/matrix/motion functions; do not mistake these for image effects |
| Data readers | Data Sample, Ramp Sample, Table Lookup and Point Set Read backend/persistence | Named typed picker and ordinary desktop create/edit workflow |
| Graph organization | Reroute and presentation frames | Reusable groups with typed exposed ports |
| Viewer | Session inspection, channels, EV/gamma and pixel probe | Extend the existing service where useful, rather than duplicate render truth |

Registry evidence: `src/document/node_definition_registry.cpp`, `value_node_definitions.cpp`,
`value_utility_nodes.cpp`. The closed operation/effect variants in
`src/runtime/include/bloom/runtime/compiled_plan.hpp` distinguish implemented pixel operations from
catalogue proposals. Existing tests were inspected/located; their presence is not a fresh passing gate.

Before expansion, complete [existing correctness and workflow repairs](user-control-policy-audit.md),
including Add-search eligibility, exact numeric edits and end-to-end color intent. Data-block authoring
and missing shape parameter animation should be bounded follow-up slices rather than hidden prerequisites.

## Ordered chunks

### 1. [#332: Pointwise image effects: Exposure, Offset, Multiply, Invert](https://github.com/kinetik-gg/bloom/issues/332)

Extend the existing ImageEffectKernel and production preparation. Define scene-linear working color, alpha association and negative/HDR behavior; share CPU/GPU math. Do not introduce a separate render engine.

Independent foundation; schedule after existing correctness fixes.

### 2. [#333: Image generators: Linear Gradient, Radial Gradient, Checker](https://github.com/kinetik-gg/bloom/issues/333)

Add one coordinate-aware generator primitive with explicit interpolation space, alpha, extents, pixel aspect, animated endpoints and proxy/full agreement. Four-Point Gradient remains a later addition.

Depends on #332.


### 3. [#334: Image Mix, Set Alpha and Multiply Alpha](https://github.com/kinetik-gg/bloom/issues/334)

Implement explicit image/mask alignment, channel-to-coverage extraction, factor policy and output windows. Flat RGBA masking first; do not require arbitrary AOV infrastructure.

The first flat-RGBA slice uses two-image Image Mix with an animated unit factor, and Set Alpha
with a second matte image and an Alpha/Red/Green/Blue/Luma selector. The first image owns the
output data and display windows and pixel aspect. The second image is sampled at the same
composition-space integer pixel coordinate, with transparent black outside its data window.
Mix interpolates premultiplied RGBA by its factor. Set Alpha replaces coverage while preserving
the first image's straight RGB; a zero-alpha first pixel has zero RGB even if the matte gives it
positive coverage. RGB matte channels are unpremultiplied and scene-linear Rec. 709 luma is read
from the premultiplied colour (#460), then clamped to [0,1]; a zero-alpha matte gives zero
coverage. A separate per-pixel mask input for Mix remains part of the later Matte integration
rather than this two-image slice.

Depends on #332.


### 4. [#335: Canvas operations: Crop, Pad, Flip and Rotate 90](https://github.com/kinetik-gg/bloom/issues/335)

Reuse display/data-window and exact discrete-mapping helpers; test offset origins, negative windows, pixel aspect and avoid accidental filtering.

Independent foundation; schedule after existing correctness fixes.

### 5. [#336: Resize, Fit and standalone Transform 2D](https://github.com/kinetik-gg/bloom/issues/336)

Reuse Layer affine/resampling kernels with explicit extent/filter controls. Test degenerate transforms, nonuniform scale, ROI and preview/final agreement.

Depends on #335.


### 6. [#337: Gaussian Blur, Box Blur and Matte Blur](https://github.com/kinetik-gg/bloom/issues/337)

One separable neighborhood primitive, halo/ROI planning, explicit edges and alpha/coverage behavior. Gaussian Blur, Box Blur and Matte Blur (a Mask-to-Mask Coverage variant of the same kernel) are implemented. This is the dependency for shadows/glow, not three separate engines.

Depends on #334.


### 7. [#338: Luma Key, Matte Erode and Matte Dilate](https://github.com/kinetik-gg/bloom/issues/338)

Reuse explicit color/channel coverage and morphology. Test HDR/partial alpha, inverted keys, image boundaries, radius changes and cancellation. Implemented: Luma Key (Image to Mask, track-matte Rec. 709 luma) and Matte Erode/Matte Dilate (minimum/maximum variants of the #337 separable neighborhood over a Mask).

Depends on #334, #337.


### 8. [#339: Drop Shadow, Glow and Light Wrap recipes](https://github.com/kinetik-gg/bloom/issues/339)

Compose existing matte/threshold/filter/offset/composite primitives. Separate Light Wrap until foreground reconstruction semantics are ready; do not clone blur code. Implemented: each node lowers to a chain of the existing primitives (the separable blurs, Transform 2D, Luma Key, Set Alpha, Multiply Alpha, Image Mix, Levels and a Merge entry with an explicit blend mode) with derived values in the value graph; Glow has a threshold above 1 with a knee, an optional key pass and Mask and multi-octave falloff; Light Wrap takes a background input; all three are premultiplication-correct and HDR-safe. #506 adds Glow's Physical (energy-conserving inverse-power octaves) and Streak (N-point star and anamorphic streaks of chained Directional Blur rays) modes and its shared per-channel radius, gradient, lens-dirt and highlight roll-off controls on the same node (see [image filters](../architecture/image-filters.md#glow-modes)).

Depends on #337, #338.


### 9. [#340: Levels, Curves and image Hue/Saturation](https://github.com/kinetik-gg/bloom/issues/340)

Build on pointwise effects and usable typed curve/ramp editing. Explicit color model and HDR/alpha behavior. Prerequisite: data reader block picker/create/edit workflow and color audit corrections. Implemented: per-channel Levels and Curves (RGB master, R/G/B/A; clamp off by default; Curves are PCHIP point lists with linear HDR extrapolation, edited in a graph with hue curves since #493; Levels edited by histogram handles on its card since #494) and Hue/Saturation (Filter Effects hue-rotate and saturate matrices), with no colour transform so Raw data passes grade as stored. #494 adds Grade (Lift/Gamma/Gain/Offset colour wheels and a primaries bar, interactive on the node card; see [image filters](../architecture/image-filters.md#grade)). The optional mask input waits on #383.

Depends on #332.


### 10. [#341: Scalar Sine, Cosine and Power; separate deterministic motion utilities](https://github.com/kinetik-gg/bloom/issues/341)

First implement exactly Sine/Cosine/Power using checked primitives. Follow-up bounded sub-slice Oscillator/Wiggle/Loop Value uses versioned noise and arbitrary-time evaluation. No general expression engine.

Independent foundation; schedule after existing correctness fixes.

### 11. [#342: Reusable Group Input, Group Output and Node Group](https://github.com/kinetik-gg/bloom/issues/342)

Typed exposed ports, nested dependency identity, parameter ownership, commands, persistence and cycle checks. Existing organizational frames remain presentation-only.

Independent foundation; schedule after existing correctness fixes.

### 12. [#343: Static SVG Vector Asset import and Vector Source](https://github.com/kinetik-gg/bloom/issues/343)

Delivered as native layers rather than a picture: every element a Shape layer driven by its own Path node, groups as nested Merges, gradients, clips, masks and dashes from existing nodes, text as Text layers, imported by a bounded Qt-free reader. Design, profile, limits and what is reported: [`architecture/vector-artwork.md`](../architecture/vector-artwork.md).

Independent foundation; schedule after existing correctness fixes.

### 13. [#344: Path processing: Transform Path, Trim Path and Offset Path](https://github.com/kinetik-gg/bloom/issues/344)

Introduce minimum typed path-flow boundary, stable subpath identity and robust length/join/offset diagnostics. Dash/Repeater/Boolean Paths are a separate follow-up chunk.

Depends on #343.


### 14. [#345: Shaped Text Layout, Text Animator and Text On Path](https://github.com/kinetik-gg/bloom/issues/345)

Portable shaping/layout with stable glyph identity and selector semantics. Reuse current Text Source controls; Text To Shape follows path/layout maturity.

Depends on #344.


### 15. [#346: Image Time Offset, Frame Hold and Time Remap](https://github.com/kinetik-gg/bloom/issues/346)

Generic image-input time mapping reusing exact rational/source mapping; temporal dependency identity and seek behavior. Echo/Trails/transform Motion Blur remain a separate follow-up chunk.

Independent foundation; schedule after existing correctness fixes.

### 16. [#347: Audio Amplitude, Audio Waveform and Audio Spectrum](https://github.com/kinetik-gg/bloom/issues/347)

Prepared cancellable analysis, exact time and source revision caches. Reuse media services after #329 audio repair; no general DAW graph.

Independent foundation; schedule after existing correctness fixes.

### 17. [#348: Corner Pin, ST Map and Displace](https://github.com/kinetik-gg/bloom/issues/348)

Shared inverse-mapping/gather primitive, explicit coordinates, filtering, border/ROI and invalid-map behavior. Keep absolute UV and relative displacement distinct.

Depends on #336.


### 18. [#349: Histogram, Image Statistics and Compare](https://github.com/kinetik-gg/bloom/issues/349)

Worker reductions and existing Viewer/readout services. Add standalone nodes only where durable connected output is useful; inspection alone must not duplicate project render truth.

Depends on #332.


## Workflow milestones

First artist-facing milestone combines pointwise grades, gradients, flat-RGBA masking and separable
blur in one saved animated composition. Shadow/Glow then reuse those primitives. Keep each assignment
to its named 3–5 nodes/capabilities; a new framework or adjacent family requires a separate chunk.

Every image-producing node needs schema/discoverable controls, commands/undo/save-reopen, CPU reference
semantics for alpha/HDR/negative values/windows/ROI, actual production GPU parity across preview and
final routes, source/color/time/quality cache identity, cancellation, budget/retirement behavior and
cold/warm RAM-preview evidence. An unavailable device reports explicit Unsupported/CPU fallback or
NotRun; a CPU-rasterized upload is not GPU pixel implementation. Preserve equivalent platform semantics
or an explicit supported fallback. Group expensive checks locally at these milestones, never per node.

Advanced temporal/flow, arbitrary-channel/AOV/deep, tracking/solving, 3D, particles and general audio
remain later families with prerequisites. Their catalogue presence does not make them quick additions.

