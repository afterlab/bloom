# Evaluation Values And Primitive Semantics

Status: working

Updated: 2026-11-01

## Purpose And Scope

Bloom separates artist-facing node definitions from backend-neutral execution primitives. A node
defines stable authoring schema and user intent; its lowering recipe composes checked primitives
into an immutable evaluation plan.

This document owns internal numeric, value, color, alpha, lowering, and conformance semantics. It
does not own node presentation, external interchange policy, or display UI. Those remain with the
layer/graph, standards, and Viewer contracts.

```text
Artist node or structured layer property
        |
        v
Versioned NodeDefinition + parameter/port schemas
        |
        v
Snapshot compiler and lowering recipe
        |
        v
Typed immutable runtime operations
        |
        +---------------------+
        v                     v
Checked domain primitives   Scheduling metadata
        |                   ROI / halo / time / cost
        +----------+----------+
                   v
          CPU reference / GPU provider
```

One checked primitive should power layer controls, graph nodes, scripting, CPU evaluation, and GPU
evaluation. Those surfaces must not develop subtly different versions of the same operation.

## Value Layers

Bloom keeps these layers distinct:

1. Authoring values are durable parameter values such as Float64, `Vec2d`, and straight `Color4d`.
2. Compiled values are validated immutable values bound to exact operations and precision.
3. Canonical process pixels are premultiplied Float32 RGBA in an image with explicit color identity.
4. Prepared display pixels are presentation results and never replace process pixels or authoring
   truth.

Conversions between layers are explicit, checked, and covered by their own semantics. The current
`Image` socket is only the first transport kind; semantic role constraints must exist before masks,
depth, normals, motion, UV, ID, or arbitrary data images can use it safely.

## Ownership

- `src/core` owns allocation-free scalar and small-value math that does not depend on images, Qt,
  project state, ambient color configuration, or a render device.
- `src/render` owns image descriptors, sampling, compositing, filtering, channel, alpha, tile, and
  color-aware image kernels.
- `src/runtime` owns frozen node definitions, snapshot lowering, typed plans, scheduling contracts,
  diagnostics, and backend-neutral evaluator orchestration.
- Optional modules own coherent geometry, 3D, material, audio, or pipeline primitives. A library
  boundary is created only when a concrete implementation requires it.
- `src/ui` presents schemas and results; it never owns primitive semantics.

## Type Binding

Storage resemblance does not imply semantic interchangeability:

- `Color` is not `Vec4`.
- `Mask` is not a grayscale color image. `SocketValueKind::Mask` (ADR 0028) carries single-channel
  coverage in `[0, 1]` over composition pixel space, transported privately as premultiplied white
  (every channel holds the coverage; consumers read alpha; zero outside its data window). It
  connects only to Mask inputs, with no socket conversion to or from Image in either direction. The
  Mask node produces one, and Luma Key computes one from an Image's luma
  ([image filters](image-filters.md#luma-key)); Image Mix's optional `mask` input consumes one
  (factor × mask per pixel); Matte Blur, Matte Erode and Matte Dilate filter one into another
  ([image filters](image-filters.md#matte-blur)).
- Depth, normals, motion vectors, UVs, object IDs, and arbitrary data channels are not color.
- A uniform value, per-element field, image grid, and variable-length list are different shapes even
  when their element type matches.
- Point, direction, normal, and generic vector values have different transform behavior.

There are no implicit Bool-to-number, Vec-to-Color, Color-to-luminance, straight-to-premultiplied,
color-to-data, domain, unit, or coordinate-space conversions. A node definition requests the exact
kind or inserts an explicit conversion operation.

Durable scalar authoring values bind to Float64 until a checked conversion at an image/kernel
boundary. Canonical process pixels bind to Float32. Node registry parameter matching remains exact;
type erasure and per-pixel string lookup are forbidden inside the hot evaluator.

## Scalar Primitive Vocabulary Version 4

`bloom_core` defines checked Float32 and Float64 primitives. Semantics version `4` is an append-only
extension of versions `1`, `2` and `3`: the original enum values, stable IDs, operand orders, formulas, and errors
remain unchanged.

The version 1 base is:

| ID | Operation | Operands | Semantics |
| --- | --- | --- | --- |
| `bloom.core.scalar.add` | Add | `left`, `right` | finite sum |
| `bloom.core.scalar.subtract` | Subtract | `left`, `right` | finite difference |
| `bloom.core.scalar.multiply` | Multiply | `left`, `right` | finite product |
| `bloom.core.scalar.divide` | Divide | `numerator`, `denominator` | both signed zeros are invalid denominators |
| `bloom.core.scalar.multiply-add` | Multiply Add | `multiplicand`, `multiplier`, `addend` | one deliberate fused operation |
| `bloom.core.scalar.minimum` | Minimum | `left`, `right` | `+0/-0` tie produces `-0` |
| `bloom.core.scalar.maximum` | Maximum | `left`, `right` | `+0/-0` tie produces `+0` |
| `bloom.core.scalar.clamp` | Clamp | `value`, `minimum`, `maximum` | closed interval; equal bounds valid; reversed bounds fail |
| `bloom.core.scalar.remap` | Remap | `value`, `sourceMinimum`, `sourceMaximum`, `destinationMinimum`, `destinationMaximum` | reversed ranges and extrapolation allowed; equal source bounds fail |
| `bloom.core.scalar.mix` | Mix | `start`, `end`, `factor` | exact endpoints and extrapolation; never clamps |

Version 2 adds this color-agnostic tranche:

| ID | Operation | Operands | Semantics |
| --- | --- | --- | --- |
| `bloom.core.scalar.absolute` | Absolute | `value` | non-negative magnitude; both zero signs produce `+0` |
| `bloom.core.scalar.negate` | Negate | `value` | reverses the sign, including signed zero |
| `bloom.core.scalar.sign` | Sign | `value` | negative values produce `-1`, positive values produce `+1`, and zero retains its sign |
| `bloom.core.scalar.reciprocal` | Reciprocal | `value` | `1 / value`; both zero signs fail as divide-by-zero |
| `bloom.core.scalar.square-root` | Square Root | `value` | principal square root; strict negatives fail outside the domain and `-0` produces `-0` |
| `bloom.core.scalar.floor` | Floor | `value` | greatest integral value no greater than the input |
| `bloom.core.scalar.ceiling` | Ceiling | `value` | least integral value no less than the input |
| `bloom.core.scalar.round` | Round | `value` | nearest integral value; halfway cases tie to an even integer |
| `bloom.core.scalar.truncate` | Truncate | `value` | integral value toward zero |
| `bloom.core.scalar.fraction` | Fraction | `value` | signed truncation-relative fractional part in `(-1, 1)` |
| `bloom.core.scalar.modulo` | Modulo | `dividend`, `divisor` | truncating remainder with the dividend sign; both zero divisor signs fail |
| `bloom.core.scalar.step` | Step | `edge`, `value` | `0` below the edge and `1` at or above it |
| `bloom.core.scalar.smoothstep` | Smoothstep | `lowerEdge`, `upperEdge`, `value` | clamped interval factor followed by `t * t * (3 - 2 * t)` |
| `bloom.core.scalar.smootherstep` | Smootherstep | `lowerEdge`, `upperEdge`, `value` | clamped interval factor followed by `t * t * t * (t * (t * 6 - 15) + 10)` |

Version 3 adds the transcendental tranche. Angles are radians; the operand name says so.

| ID | Operation | Operands | Semantics |
| --- | --- | --- | --- |
| `bloom.core.scalar.sine` | Sine | `angleRadians` | `sin` of the angle; `-0` produces `-0`; every finite angle is in domain |
| `bloom.core.scalar.cosine` | Cosine | `angleRadians` | `cos` of the angle; every finite angle is in domain |
| `bloom.core.scalar.power` | Power | `base`, `exponent` | `base` raised to `exponent`; `0^0` is `1`; a negative base needs an integral exponent (otherwise outside the domain); a zero base with a negative exponent is a divide-by-zero pole; overflow is a non-finite result |

Version 4 adds Log, Exp, Tangent and the degrees forms of the three trigonometric operations.
Every earlier operation, including Sine and Cosine in radians, is unchanged.

| ID | Operation | Operands | Semantics |
| --- | --- | --- | --- |
| `bloom.core.scalar.log` | Log | `value` | natural logarithm; a negative value is outside the domain; zero is a divide-by-zero pole |
| `bloom.core.scalar.exp` | Exp | `exponent` | `e` raised to the exponent; overflow is a non-finite result and a very negative exponent underflows to `0` |
| `bloom.core.scalar.tangent` | Tangent | `angleRadians` | `tan` of the angle; `pi / 2` is not exactly representable, so its tangent is a very large finite number, not an error |
| `bloom.core.scalar.sine-degrees` | Sine (Degrees) | `angleDegrees` | sine of an angle in degrees, reduced exactly in degrees first: `sin(0)`, `sin(180)` are `0`, `sin(30)` is `0.5`, `sin(90)` is `1`; `-0` produces `-0` |
| `bloom.core.scalar.cosine-degrees` | Cosine (Degrees) | `angleDegrees` | cosine of an angle in degrees, exact at the multiples of 30 and 90 (`cos(60)` is `0.5`, `cos(90)` is `0`) |
| `bloom.core.scalar.tangent-degrees` | Tangent (Degrees) | `angleDegrees` | tangent of an angle in degrees; `tan(45)` is `1`; the odd multiples of 90 are exact poles and report divide-by-zero |

Sine, Cosine and Power call the platform C library in the operand's own precision. Their
conformance policy is: the domain checks above and the exact identities (`sin(0)`, `cos(0)`,
`x^0`, `x^1`, `0^positive`) are frozen and tested; interior results are accurate to within a few
ulps and are not promised bit-identical across C libraries, so a value that feeds a cached render
may differ in its last bits between platforms. Log, Exp and Tangent follow the same policy.

Primitive IDs identify evaluator kernels, not durable artist-facing node types. Adding an ID may
extend the version; changing an existing operation's operand order, precision, formula, validation,
or error behavior requires a new semantics version. Old compiled or cached behavior is never
silently reinterpreted.

The version is `2` even though every version 1 operation retains its exact contract because the
closed primitive vocabulary is itself part of compiled-plan and cache compatibility. A consumer
that recorded version `1` must be explicitly supported as version `1` or recompiled; it cannot
infer version `2` merely because the particular operation it uses predates the extension.

Only exact `float` and `double` inputs are accepted. All operands must be finite; subnormals and
signed zero are valid except for a zero denominator. Result or intermediate NaN/infinity is a
failure, never saturation. Evaluation requires the declared round-to-nearest environment and
supported subnormal behavior; an incompatible ambient floating-point environment fails explicitly.
Implicit contraction and fast-math reassociation are prohibited. Only operations whose contract
names a fused result may call an explicit fused primitive.

Validation precedence is known primitive, exact arity, all-input finiteness, floating-point
environment, operation-domain checks, then result finiteness. Failures expose no plausible numeric
fallback. The code-level error vocabulary distinguishes unknown primitive, invalid arity,
unsupported environment, non-finite input, divide-by-zero, invalid interval, degenerate range,
non-finite result, and an input outside an operation's mathematical domain.

Version 2 freezes these additional details:

- Absolute, Negate, Sign, Floor, Ceiling, Round, Truncate, Fraction, Modulo, and Square Root retain
  the signed-zero behavior stated in the table. In particular, rounding or fraction results that
  approach zero from below retain `-0`. Step and the two smooth functions instead return canonical
  `+0` or `+1` at their closed boundaries.
- Finite subnormal inputs are accepted and are never intentionally flushed. A finite subnormal
  result is successful. Reciprocal of a nonzero value fails only when its rounded result is
  non-finite; therefore reciprocal of the smallest subnormal reports a non-finite result rather
  than divide-by-zero.
- Square Root is the principal result rounded to the bound precision under the required
  round-to-nearest environment. `-0` is in-domain; every finite value less than zero is not.
- Floor, Ceiling, Round, and Truncate return a floating value in the input precision, not an integer
  conversion. Round is round-to-nearest, ties-to-even and uses the required ambient rounding mode.
  Fraction is the signed fractional result returned by splitting at Truncate; integral negative
  inputs therefore produce `-0`.
- Modulo is the finite truncating remainder corresponding to a quotient rounded toward zero. Its
  magnitude is less than the divisor magnitude, and an exact-zero result retains the dividend's
  sign. No Euclidean non-negative adjustment is performed.
- Step compares `value < edge`; signed zeros compare equal and therefore select `1`.
- Smoothstep and Smootherstep require `lowerEdge < upperEdge`. Equal edges report a degenerate
  range; reversed edges report an invalid interval. Values at or below the lower edge return exact
  `+0`, and values at or above the upper edge return exact `+1`. Interior normalization uses the
  robust version 1 Remap factor, including opposite-sign extreme bounds. The polynomial expressions
  are evaluated in the parenthesized order printed in the table, with separate unfused operations;
  the compiler may not contract them.

Wrap is deferred until half-open endpoint and
extreme-range reduction behavior is frozen. Snap is deferred until its anchor, signed increment,
halfway tie, and overflow rules are frozen. Omitting these operations is preferable to exposing
backend-dependent math under a stable ID.

These are kernels, not artist-visible node records. Artist nodes add typed ports, defaults, units,
animation roles, UI metadata, and lowering without changing the kernel math.
Evaluation is `noexcept`, allocation-free, and has no mutable or hidden service state. It reads the
ambient floating-point controls only to validate them before authored arithmetic.

## Animation Sampling Version 1

Animation interval selection is exact rational work, not scalar floating-point work. Runtime first
orders and locates keys using normalized `RationalTime`, derives an exact interval factor, and rounds
that factor once to binary64 with round-to-nearest, ties-to-even. Hold returns the left value;
Linear applies Float64 scalar Mix version 1 to the scalar or each `Vec2d` component. Exact key times
and endpoints return stored values or exact factors without an avoidable arithmetic round trip.

The sampling semantics version is `1` and participates in compiled-plan compatibility and cache
identity. Full curve ownership, extrapolation, commands, diagnostics, and the portable fixed-width
rational conversion contract are defined in
[`animation-and-time.md`](animation-and-time.md).

## CPU Image Primitive Vocabulary Semantics Version 8

`bloom_render` now provides the allocation-free CPU reference row kernels used by the first
composition evaluator. Their semantics version is `8`; the evaluator and process-frame cache
identity record that version explicitly. Version 3 added the text coverage kernel and the glyph
rasterizer below; version 4 replaced the translate-only layer resample with the affine one described
under "Layer Transform Resampling"; version 5 added the per-mode blend kernel described under
"Blending"; version 6 adds the path coverage and stroke outlines below; version 7 adds affine vector coverage; version 8 (#545) gives every affine resample that is not a pure translation the GPU kernel's double-float sample-coordinate rule, described under "Layer Transform Resampling". One number covers them all: neither the rasterizer, the resampler, nor the
blend kernel carries a second semantics version that could drift out of the identity a published
frame records.

- Solid, Text, and Shape authoring colors are straight `Color4d` values stored numerically in the
  effective project/composition working space. The authoring metadata remains distinct from the
  process-image identity. The picker presents sRGB-encoded editing values and converts through the
  selected OCIO config's `sRGB - Texture` space; the evaluator receives only the validated working
  space and resulting straight value. The conversion validates the authored value, multiplies RGB
  by alpha in Float64, then performs one checked Float32 conversion. Alpha that is authored as zero
  or rounds to Float32 zero produces exact transparent black.
- The layer transform and opacity are validated once per operation. Bilinear sampling gathers
  premultiplied pixels, uses transparent taps outside the source data window, preserves exact
  integer/zero/one endpoints, and applies opacity to all four sampled components. See "Layer
  Transform Resampling" below.
- Source-over consumes separate source and in-place destination rows. The first Layer Stack entry is
  topmost, so evaluation visits stack entries in reverse order and folds bottom-to-top. Process RGB
  is never clamped.
- Blending takes the same two rows plus the layer's own blend mode, and is what the Layer Stack stage
  actually calls; source-over remains the kernel the `Normal` mode delegates to, unchanged. See
  "Blending" below.
- The qualified display boundary consumes the evaluation intent's working-space id and OCIO config
  revision. The historical reference mapper remains the byte-compatible path for
  `lin_rec709_scene`; generalized projects use the selected working-space-to-display transform and
  produce straight packed RGBA8. Its prepared display product and identity are distinct from process
  evaluation.
- Text coverage is rasterized from glyph outlines into 8-bit area coverage, then composited by a row
  kernel that scales an already-premultiplied process pixel by `coverage / 255`. **A coverage byte is
  a linear area fraction, not a gamma-encoded intensity**, so it is used directly as linear alpha and
  no transfer function, sRGB curve, or gamma exponent appears anywhere between the rasterizer and the
  process pixel. Compositing coverage in a display-encoded space is the classic cause of fringed,
  too-thin text; the process space is scene-linear, so the correct result and the simple
  implementation coincide. Zero coverage is exactly transparent black and full coverage is exactly
  the unmodified process pixel, with no multiply that could round either endpoint away.
- Text authoring colors use the same straight `Color4d` values and the same historical
  `bloom.reference.linear-srgb` authoring-encoding metadata Solid colors use; their numeric values
  are interpreted in the effective working space and converted by the same Solid conversion above.
  The coverage kernel then scales that one pixel. A text color is a solid color that glyph coverage
  attenuates, which is why there is no separate text color conversion.
- Every authored arithmetic boundary requires round-to-nearest with preserved subnormal inputs and
  results. Primitive rows allocate no storage, start no threads, and expose structured failures. The
  glyph rasterizer is the one text-path exception: it allocates its coverage bitmap, so it checks the
  bitmap's byte count against the caller's budget from the computed extent BEFORE allocating anything
  and before drawing any glyph.

### Layer Transform Resampling

One inverse-mapped affine bilinear resample serves the whole Layer Output stage: translation composed
with rotation composed with scale, all turning about the authored anchor (the authoring model is in
[`layer-graph-model.md`](layer-graph-model.md), "Layer Transform"). There is no separate translate-only
kernel in the stage; the translate-only case is a path inside this one.

- **Inverse mapping.** Each output pixel center maps back to one source coordinate through the
  precomputed inverse 2x2 matrix and pivot. Nothing per-pixel computes trigonometry, divides, or
  branches on an authored value.
- **One sample-coordinate rule on CPU and GPU (#545).** A layer that does more than move, a parented
  or concatenated layer, Transform 2D, Resize/Fit and a proxy-resampled discrete transform all
  resample through the composed device matrix the GPU's `AffineBilinearV1` command receives, with
  `render::affineMatrixResampleRow()`. That row does not evaluate the inverse map in binary64 per
  pixel. It probes the closed-form inverse at the origin of the command's output window and one step
  along each axis, splits the six binary64 coefficients into Float32 (hi, lo) pairs, and rebuilds
  each pixel's coordinate as `(origin + stepPerColumn * column) + stepPerRow * row` in error-free
  double-float arithmetic (Float32 operations and `fma` only, about 48 significand bits), exactly as
  the kernel does. The transparent range test, the floor and the single Float32 rounding of the
  factor work on that pair. The CPU and GPU routes therefore pick the same taps and factors and agree
  bit for bit on all four components, alpha included, on every platform, with no device float64. A
  region-of-interest clip evaluates the same lattice over the unclipped layer window, so it gets the
  same pixels. Before version 8 the CPU used `LayerTransform::inverseMap()` in binary64 and a
  sample whose factor was exactly 0 there could come out tiny but nonzero on the GPU and pick up a
  neighbouring tap's near-zero alpha.
- **Pure moves.** A layer that only moves keeps the translate-only `layerTransformBilinearRow()` path
  below and the GPU's exact TranslationOpacity pass.
- **Quarter turns are exact.** A rotation that is an exact multiple of 90 degrees resolves to exact
  `0` and `±1` cosine and sine instead of `std::cos`/`std::sin` of a rounded radian value, so a
  quarter turn maps pixel centers onto pixel centers and interpolates nothing. The reduction modulo
  360 is exact, so an authored 450 or -90 is as exact as a 90 or 270.
- **Translate-only is bit-identical to version 3.** Unit scale on both axes together with a rotation
  that reduces to exactly zero resolves to a path that computes the pre-version-4 arithmetic and
  nothing else — the anchor is not added and subtracted, because doing so would perturb the last bit
  of a subpixel translation. The former translate-only primitive is retained in `bloom_render`,
  unchanged and uncalled by the stage, solely as the reference a test pins that path against; a copy
  in the test tree could drift apart from the shared interpolation and sampling helpers and prove
  nothing.
- **Premultiplied edges.** Taps outside the source data window are exact transparent black, and the
  process representation is premultiplied, so interpolating towards that transparent black is already
  the correct edge falloff: no unpremultiply/repremultiply round trip is involved and no edge pixel
  can carry color above its own alpha.
- **Bounds.** A layer's output data window covers its transformed local content, including content
  outside the composition; its display window stays the composition's. The transformed bounds are the integer bounding box of
  the forward image of the bilinear support box (the source data window grown by one pixel on every
  side), so they may include a pixel the resample then writes as transparent but can never exclude
  one it would write as opaque. A scaled-down, rotated, or moved layer therefore allocates and
  resamples only the pixels it can reach. The Layer Stack composites each entry over the rows and
  columns that entry's own data window occupies.
- **Empty layers.** Empty content and a layer collapsed by a scale factor of exactly zero publish
  no image. Off-composition content remains available for a parent transform to bring back. The Layer Stack treats an
  absent entry image as a layer that contributes nothing, which is exactly what compositing an empty
  layer means — not an evaluation failure.
- **Proxy.** A proxy frame is the same picture at a smaller extent, so the full-resolution transform
  is conjugated by the per-axis proxy factor rather than re-authored. With equal horizontal and
  vertical factors — every proportional proxy extent — the device transform is exactly the
  full-resolution one. With unequal factors the conjugation keeps the proxy a faithfully squashed
  picture of the full-resolution frame, a rotated layer included, rather than pretending device
  pixels are square.

### Resize And Fit

`bloom.resize` and `bloom.fit` schema 1 lower through the image-effect boundary to the shared
`ResizeFitKernel`. Resize stretches the input display canvas independently in X and Y. Fit defaults
to centered Fit (contain); Fill (cover) instead crops centered overflow. Both compute physical aspect
from the source and target pixel aspect ratios. Source data-window offsets and overscan remain
sampling coordinates; they do not redefine the source display canvas.

Both nodes expose constant width, height, pixel-aspect numerator/denominator, a filter selector,
and bypass. Fit also exposes Fit/Fill. Dimensions and ratio terms are positive integers up to
32768; zero, negative, unsupported filter/policy values, and driven/animated geometry are rejected.
The filter is **Bilinear** (0, default) or **Nearest** (1, #480). Nearest copies the one source
tap whose centre is nearest each output sample (on an exact half-way factor, the upper tap), so a
depth, normal, motion-vector, ID or Cryptomatte image keeps its stored values exactly; with a
transparent-black tap outside the source data window it never blends two values. The generic registered-node commands provide
Add, parameter edits, duplication, undo/redo, and project persistence; Properties reads these same
schemas and selectors.

The output data/display canvas starts at zero with the authored extent and PAR. A proxy scales the
extent with ceiling and derives its exact PAR to preserve the authored physical aspect. The
Composition Output still chooses the composition render extent. Fit letterboxes with transparent
black; Fill clips to the target canvas. The existing inverse-mapped affine primitive interpolates
premultiplied RGBA directly, preserving finite signed/HDR RGB and alpha, with transparent-black
taps outside the source data window. Neither filter has a downsampling antialiasing prefilter.

CPU and GPU share checked geometry. The CPU uses the existing parented affine row reference (its
nearest tap for Nearest), which since #545 is the GPU kernel's own double-float coordinate rule; GPU preparation emits resident `GpuSceneAffineCommand` work using
`AffineBilinearV1`, whose `sampling` push word selects the nearest tap (the Required coverage axis
`feature.resample.nearest`, shared with Transform 2D), including for
preview and final-render evaluation. An optional checked output descriptor on individual and batched
affine work carries the new display canvas/PAR without a pixel readback. Deferred vector inputs
materialize through existing native coverage before this resample. No CPU-rendered Resize/Fit
output is uploaded as a GPU implementation.

Cache identity covers every geometry/filter/policy value, input identity, windows/PAR, proxy and ROI;
the executor independently keys the actual affine map, output descriptor and shader. Spatial inputs
retain full support for ROI requests. CPU work checks cancellation per row and before publication,
reports row progress, and refuses invalid geometry or output-budget exhaustion. Native work uses
the existing executor allocation, cancellation, cache pin and proven-retirement ownership. Native
numeric parity, warm-cache and batch checks are included in the existing harnesses and remain
subject to their device qualification; macOS/Windows retain the explicit CPU fallback contract.

### Transform 2D

`bloom.transform-2d` schema 1 (#336) lowers through the image-effect boundary to
`Transform2DKernel` and reuses the Layer Output's raster resampling rather than a second affine
implementation. It exposes the Layer Output vocabulary: `translate` (Vec2d, full-resolution pixels,
default `{0, 0}`), `pivot` (Vec2d offset from the centre of the input's content bounds, default
`{0, 0}`), `scale` (Vec2d factors, default `{1, 1}`) and `rotation` (clockwise degrees, default `0`).
All four are animatable and each has an operand socket, so a value-graph driver can feed it.
`extent` and `filter` are constant inline selectors without sockets. Values must be finite and are
otherwise unbounded: a negative scale mirrors, rotation winds past a full turn, and a zero scale on
either axis collapses the image.

For input content-bounds centre `c`, the map is `q = c + pivot + translate + R(rotation) * S(scale)
* (p - (c + pivot))`, which is exactly a Layer Output with `anchor = pivot` and `position = c +
pivot + translate`. The CPU reference resamples with the Layer stage's own rows: the translate-only
`layerTransformBilinearRow` for a pure Bilinear move, otherwise the composed-matrix row over the
author-space matrix the GPU command uses (#545, see "Layer Transform Resampling"). It interpolates premultiplied RGBA
directly, preserves finite signed/HDR RGB and partial alpha, and uses exact transparent-black taps
outside the source data window. Quarter turns and whole-pixel translations interpolate nothing, and
proxy frames conjugate the full-resolution map by the per-axis proxy factors.

`extent` chooses the output data window. **Transformed** (0, default) keeps the full transformed
support, including overscan outside the display window, like a Layer. **Input** (1) clips it to the
input data window, and **Display** (2) clips it to the input display window. The display window and
pixel aspect always follow the input. Content bounds also follow the input, so a downstream Layer
places the same frame and the authored movement stays visible. A zero scale, or support that misses
the selected extent, publishes no image without a diagnostic, as a collapsed Layer does. Support
beyond the +/-2^24 device coordinate range, or a non-finite value, fails with an explicit diagnostic.
An identity transform (no translation, unit scale, a whole-turn rotation) with the Transformed or
Input extent is an exact passthrough. `filter` is **Bilinear** (0, default) or **Nearest** (1,
#480), with the Resize/Fit meaning; neither has a downsampling prefilter. Nearest resamples on the
CPU with the parented affine row over the same author-space matrix the GPU command uses, so both
routes pick the same tap.

GPU preparation emits the Layer stage's raster commands for the same geometry. A pure Bilinear move
becomes the exact TranslationOpacity pass, and every other map, and every Nearest map, becomes a
resident `GpuSceneAffineCommand` (`AffineBilinearV1`, its nearest sampling for Nearest) built from
the same author-space matrix. A collapsed or fully clipped transform
emits no command. Preview and every final route go through the same production scene executor, and
no CPU-rendered output is uploaded. The CPU and GPU routes share the double-float coordinate rule,
so they agree bit for bit on all four components; a Nearest tap is copied bit for bit.

Cache identity covers the sampled values, the input content-bounds centre, extent, filter, input
identity, windows/PAR, proxy and ROI. The executor independently keys the actual matrix or
translation, the windows and the shader artifact. Animated or driven values make the operation and
everything downstream time dependent. The effect input keeps full support for ROI requests. CPU
work checks cancellation per row and before publication, reports row progress, and refuses an
output that exceeds the remaining pixel budget. Native work uses the executor's existing
allocation, cancellation, cache pin and proven-retirement ownership. macOS and Windows keep the
explicit CPU fallback contract until native qualification.

### Camera Shake

`bloom.camera-shake` schema 1 (#512, group Transform) is Transform 2D driven by a seeded shake. The
compiler lowers it to the same `Transform2DKernel` (extent Input, Bilinear), so the CPU reference, the
resident GPU command, the cache identity, cancellation and budget are Transform 2D's, and the motion
blur is its `MotionPathBlurKernel` in Transform mode with the node's own shutter (the Transform 2D
motion blur vocabulary: `motionBlur`, `shutterAngle`, `shutterOffset`, `shutterPhase`,
`shutterCurve`, `shutterShape`, `quality`, `minSamples`, `maxSamples`, `jitter`). The transform's
`translate`, `rotation` and `scale` are not authored values: they are the three outputs of one value
operation, `CompiledValueShake`, which the compiler appends to the value graph, so the host resolves
them per frame (and per shutter row) exactly as it resolves a driven Transform 2D, and the CPU and
GPU routes receive the same numbers.

Parameters (schema keys `bloom.camera-shake.<role>`): `mode` (0 Handheld, 1 Impact, 2 Gate weave),
`amplitudeX` and `amplitudeY` (pixels, >= 0, default 10), `rollAmount` (degrees, >= 0, default 0.5),
`frequency` (Hz, default 2), `roughness` (0 to 1, default 0.35) and `smoothing` (0 to 1, default 0)
are animatable with operand sockets (the shake operation reads each at the frame, so a keyed
frequency is the motion's noise at that frame's own frequency: a changing frequency re-times the
noise rather than integrating a phase); `octaves` (1 to 4, default 2), `seed` (integer 0 to 999999, default 1 in the
definition; AddNode stores a seed hashed from the new node's id modulo 1000000, so two new nodes in a
project differ and the seed is an ordinary saved constant. The project's own id is a constant (1) in
today's files, so it is not mixed in: the first shake of two projects can share a seed, and Randomize
seed gives a different one. A duplicated node copies its parameters, so a copy keeps the seed and
shakes the same), `loop` (default off), `loopLength` (seconds, default 4),
`overscan` (default on) and the motion blur parameters are constants.

Outputs: `image`, `offset` (Vector 2, pixels: Transform 2D's `translate`) and `roll` (Scalar,
clockwise degrees: its `rotation`). They name outputs 0 and 1 of the shake operation, which is also
what the Transform 2D reads, so a driven parameter sees exactly the transform that was applied. The
node is compiled in the value pass (it is not a value node, but it publishes values); its operands
are resolved there like any value node's.

The motion (`core/camera_shake.hpp`) is a pure function of the settings, the time in seconds and the
frame rate: three streams (x, y, roll) of unit motion in [-1, 1], each hashing `(seed, stream,
lattice index)` with the versioned value hash (`hashWord`). Handheld is fractal value noise: lattice
values joined by a smootherstep at `frequency` points a second, octave `o` at `3^o` times the rate and
`roughness^o` of the weight, normalised by the weights so it stays within [-1, 1]. Impact adds, per
lattice point, a random push `exp(-6 s) sin(2 pi 2.5 s)` (`s` the intervals since the hit, normalised
by its peak 0.5889, summed over the last three hits and clamped to [-1, 1]). Gate weave is 0.75 a
per-frame hash (frame = `floor(t * rate + 1e-6)`) plus 0.25 of the Handheld drift. `smoothing` averages
nine samples across `smoothing / frequency` seconds centred on the time. Offset is `(amplitudeX * x,
amplitudeY * y)` and roll `rollAmount * roll`.

With `loop` on, the lattice has `max(1, round(loopLength * frequency))` points (octave `o`: `3^o` times
as many) and the time maps to `t / loopLength * points`, snapped to 1e-9, with indices wrapped, so the
motion at `t` and at `t + loopLength` is the same; Gate weave wraps its frame index by `round(loopLength
* rate)`.

`overscan` sets the scale (both axes) to `fillScale`: the smallest scale about the frame centre at which
the shaken image covers the frame for every translation within +-`amplitudeX`, +-`amplitudeY` and every
roll within +-`rollAmount`, found by mapping the frame's corners (four full-resolution pixels out, so
the resampler's blend of neighbouring pixels stays inside the image even in a preview at an eighth of
the resolution) back through each extreme translation and a 256-step sweep of the roll. It is 1 when
nothing moves. The scale follows keyed amplitudes frame by frame.

Both the motion and the cover scale are evaluated on the host in binary64, so the CPU and the GPU draw
the same transform; the GPU coverage contract classifies the node as `node.bloom.camera-shake`,
satisfied by a genuine Transform 2D resident preparation of a rolled, scaled, sub-pixel shaken move.
No pixel operation of its own exists, so there is nothing to opt out.


### Blending

The Layer Stack stage folds each entry through one blend kernel carrying that entry's own Layer Output
blend mode. The mode vocabulary, every formula, and the premultiplied compositing fold are owned by
[`color-management.md`](color-management.md), "Blend modes"; what belongs here is the primitive's
contract.

- **Ordering and blending are separate.** The stack says which layer is above which and is still
  visited in reverse so the first entry is topmost; the mode comes from the Layer Output the entry
  names, so reordering two layers and re-blending one are independent edits.
- **A plain image entry may name its mode.** An entry with no Layer is Normal unless it carries an
  explicit mode (`CompiledMergeInput::blendMode`, #339), which only the compiler's recipes write:
  Glow adds its glow with `Add`, Light Wrap composites its wrap `Atop` (or `In`) the foreground. A
  Layer entry never carries one. The mode is keyed with the entry and reaches the same per-entry
  BlendV1 fold on the GPU route; its absence keeps every existing key.
- **Alpha is source-over under every separable mode.** For `Normal` through `Divide` only the color
  combination varies, computed with exactly the alpha expression the source-over kernel uses, so
  such a mode never changes how much of the backdrop a layer covers. The channel extremes (`Minimum`,
  `Maximum`) and the Porter-Duff mattes (`In` through `Mask`) own their alpha.
- **`Normal` is bit-identical to version 4.** The kernel delegates to the retained source-over row for
  that mode rather than re-deriving it, so a composition whose every layer is `Normal` produces the
  same bits it did before blend modes existed. The semantics version still moves, because the same
  plan value can now mean a different picture. Appending modes after the first eight changes no
  existing picture, so it did not move the version again.
- **`Add` needs no round trip.** Its separable function reduces the general fold to premultiplied
  addition exactly; the other separable modes unpremultiply, apply the mode, and re-premultiply, with
  Float64 intermediates and one Float32 rounding at the end. The extremes and the mattes work on
  premultiplied components directly: exact min/max, or two exact Float64 products summed and rounded
  once.
- **Alpha endpoints are exact.** Under a separable mode an alpha-zero source is skipped and leaves the
  backdrop untouched; an alpha-zero backdrop takes the source pixel through unchanged, which is what
  the fold collapses to. Source-over's other shortcut -- an opaque source replacing the destination --
  does NOT generalise and is deliberately absent, because every mode but `Normal` still reads the
  backdrop's color at full source alpha. The extremes and the mattes have no shortcut: every pixel
  takes the formula, and a zero-alpha result is transparent black.
- **Some modes reach past the layer.** `Minimum`, `Maximum`, `In`, `Out` and `Mask` change the
  backdrop under a transparent source, so the stage folds them over every row and column of the stack
  window with a transparent source outside the layer's data window
  (`core::blendModeKeepsBackdropUnderTransparentSource`). Every other mode walks only the rows and
  columns the layer occupies. An entry that published no image for the frame is skipped under every
  mode.
- **Nothing is clamped.** A negative or HDR channel survives the blend, and a non-finite or
  finite-overflow result fails the row with a typed error rather than being clipped.
- **The blend mode is a discrete authored value.** It carries no curve: the schema declares it
  non-animatable, so the compiled plan holds a resolved enumerator rather than a parameter source.

### Text Rasterization And Local Layout

Text rasterization is Qt-free and deterministic. `bloom_render` embeds four vendored faces as
build-time byte arrays: DejaVu Sans Book, Inter Regular, Inter Medium, and Inter SemiBold. A text
font is a non-animatable reference to a `Font` asset, never embedded project bytes. The compiler
resolves that asset through the platform catalogue, reads a bounded system file at compile time,
verifies its digest, and registers the parsed face once per digest in the glyph registry. Evaluation
never reads a font from the filesystem. A missing or changed reference emits a warning and uses
embedded DejaVu Sans while preserving the asset record for relink or replacement. A pre-reference
text node with no binding still produces the old pixels. Outline rasterization is the vendored
`stb_truetype` header, compiled into one translation unit behind a narrow Bloom-owned adapter; its
acquisition provenance, license review, and security review are in
`dependencies/licenses/stb_truetype/`, and that security review qualifies the library only for font
bytes Bloom itself pins. Each embedded file has a configure-time byte-count assertion.

Text v2 lays out multiple lines using glyph advances and kerning plus authored letter spacing.
Without a box, each line is aligned Left, Center, or Right within the widest line and the glyph union
is the local bounds. With a box, optional word wrapping uses the box width, horizontal alignment is
within the box, vertical alignment positions the line stack, and Clip or Grow determines whether the
coverage window is clipped to the box. Point text's anchor mode shifts the baseline anchor without
changing its content-sized bounds. Line baselines are separated by em size times line height and
snapped to integer rows. Empty lines advance layout without inventing ink. CRLF is accepted. Shaping
and bidi remain deferred.

Solid v2 evaluates its required dimension operands
into exact local bounds and uses a containing integer buffer, with fractional edge coverage.

Empty content, and content whose glyphs are all blank, rasterize to no coverage and compose a
transparent frame. That is a success, not a failure: a text layer an artist has not typed into yet is
a real, selectable, editable layer. A codepoint the face does not cover resolves to the face's own
missing-glyph box, so unsupported text is visibly missing rather than silently dropped, and content
that is not well-formed UTF-8 is refused at the document boundary rather than rendered as
replacement boxes.

Layer v4 maps `centre(bounds) + anchor` to position in composition space. The plan carries required
dimension and layout operands and one placement contract. At the sampled frame time, the worker
retains each operation's local and output rectangles and each layer's transformed corners and anchor.
These values accompany the immutable ProcessFrame and operation-cache result; the viewer reads them
without evaluation or rasterization. They are derived state and never persisted as project truth.

Merge v2 takes the union of transformed input bounds and retains its intermediate image outside the
composition. Composition Output crops to the composition window. Source bounds are distinct from the
bilinear support window so filtering padding never shifts an anchor. Zero scale and empty glyph
content produce empty geometry.

Preflight validates operand references and schema domains. Actual intermediate image allocations also
check the remaining request pixel budget, including live text coverage. Cancellation is checked at
operation and scanline boundaries, and cancelled/failed evaluation publishes no partial frame.
Operation memoization includes dimension, typography, and dependency values and retains matching
evaluated geometry. Display preparation remains a separate typed stage.

Evaluator semantics 10, primitive semantics 7, plan semantics 9, and animation semantics 2 are the
current identities; COLOR-3 moved the plan/evaluator pair to 8/9 for image-effect lowering, premultiplied
pointwise-effect evaluation moved the evaluator to 10, and ADR 0024 moved the plan to 9 (operand
sampling and media selection are relative to an operation's time clock; Layer Output ranges are in
placement-clock time) with no pixel change. TEXT-2 moved the plan version for the text reference/layout grammar and SHAPE-1
moved the evaluator and primitive versions for path rasterization; the two lanes landed together, so
all three steps are reflected in one set of re-derived identity goldens. Existing point-text pixels
remain pinned because the zero-box defaults take the old path; only new box-layout pins were added.

### Path coverage

`src/render/path_raster.cpp` owns the Qt-free path reference primitive. Coordinates stay in
full-resolution author space. Cubic subdivision bounds deviation and control-polygon excess to
1/32 of an output pixel using the larger per-axis proxy scale. Subdivision is capped at depth 20
and generated geometry at 262,144 points; pathological geometry fails with a diagnostic. The
reference evaluator checks the floating-point environment before invoking these kernels.

Coverage is deterministic 4×4 center sampling, with half-open scanline crossings and winding or
parity accumulation. The byte is `(coveredSamples * 255 + 8) / 16`, a linear area fraction.
Analytic integration is deliberately deferred: the fixed grid gives a small, independently
verifiable CPU contract. Rectangle corners and ellipses use cubic quarter arcs; ellipses use four
cubics with kappa 0.5522847498307936. Polygon corner radii use tangent circular arcs represented by
cubics, constrained to half the adjacent edge lengths. Polygon and Star extrema are fitted to
the authored bounding box.

A stroke builds consistently wound outline polygons for segment strips, joins and caps, then fills
their union. Miter joins have a fixed limit of four half-widths and fall back to Bevel; Round joins
and caps flatten to the same tolerance. Inside and Outside closed strokes use a full-width outline
on each side and intersect coverage samples with the fill region or its complement. This defines
alignment even at self-crossings under either fill rule. Open strokes use Center. Degenerate
zero-length paths have no coverage. Bounds include visible stroke extents; Inside uses the path
bounds. Fill and stroke share `coverageSolidRow`, with stroke composited source-over afterwards.

Flattening, outline construction and coverage scanlines check cancellation. Evaluation reports the
normal operation progress and accounts for concurrent coverage/stroke rows before allocating the
content-sized process image. Empty paths are successful empty images. The reference tests compare
square, circle and five-point-star coverage with independent geometric predicates, and pin cap,
alignment, fill-rule, proxy and cancellation behavior. Runtime tests pin rectangle, circle, star,
stroked-line and even-odd-path pixels and animation.

SHAPE-1 advances image primitive semantics 5 → 6 and evaluator semantics 6 → 7; TEXT-2 advances
plan semantics 4 → 5 in the same integration, and animation semantics stay 2. The independent
output-identity oracle reproduces every prior identity golden set -- including each lane's own
pre-merge set -- before deriving the single combined replacement; existing pixel goldens remain
unchanged and the new text and shape pixel pins keep passing.

## Primitive Families

| Family | Owner | Examples | Priority |
| --- | --- | --- | --- |
| Scalar and control | `src/core` | checked arithmetic, remap, interpolation, comparison, select | Foundation |
| Vector and transform | `src/core` | vector math, matrices, affine/projective transforms | Essential |
| Color values | `src/core` plus color service contracts | tagged color intent, luminance models, color-aware interpolation | Essential; never generic vector math |
| Image composition | `src/render` | fill, copy, premultiply, unpremultiply, source-over, blend | First pixels |
| Sampling and spatial | `src/render` | nearest/bilinear sampling, affine warp, crop, resize, borders | First pixels |
| Channels and masks | `src/render` | extract/combine/copy channels, coverage math, morphology | Essential compositor |
| Path coverage | `src/render` | bounded cubic flattening, winding/parity coverage, filled stroke outlines | Built |
| Text coverage | `src/render` | glyph outline rasterization to 8-bit area coverage, coverage-scaled solid composition | First pixels |
| Neighborhood filters | `src/render` | blur, sharpen, convolution, halo computation | Essential compositor |
| Reduction and analysis | `src/render` | bounds, histogram, statistics, tracking inputs | Advanced |
| Temporal | `src/runtime` and `src/render` | time mapping, frame blending, motion, bounded history | Motion/VFX |
| Procedural | `src/core` or owning domain | versioned hash, random, noise, paths; field programs ([fields.md](fields.md)) | Motion design |
| Optional domain | contributing module | geometry fields, 3D scenes, materials, simulations | Pipeline-specific |

## Color And Alpha Contract

Color-sensitive primitives require a declared color context. Generic scalar and vector kernels do
not accept color values and never invoke color management implicitly.

An image or color evaluation boundary carries:

- a resolvable color-encoding identity, including the declared scene-linear role where required
- semantic channel roles distinguishing color from alpha, masks, depth, normals, motion, UVs, IDs,
  and other data
- straight or premultiplied alpha association
- data and display windows, pixel aspect, and coordinate convention where relevant
- the processing space selected by the project/composition working-space plan, render request, or
  explicit color-transform operation

`Color4d` is a straight authoring value. RGB may be negative or HDR; alpha is finite in `[0, 1]`;
its owning schema supplies encoding and role. `Color4d` performs no conversion and is never passed
to scalar primitives as an opaque four-component number. The initial Solid schema binds its value
to authoring encoding `bloom.reference.linear-srgb`; its version 1 lowering policy declares the
numeric identity conversion to the process space. Authoring metadata is not rewritten to
`lin_rec709_scene` merely because that conversion is currently an identity.

Canonical `Rgba32f` process storage is premultiplied Float32 RGBA. RGB remains finite and unclamped,
alpha remains finite in `[0, 1]`, and alpha zero canonicalizes RGB to exact zero. The frame's
`EvaluationColorIntent` carries the working-space id and OCIO content revision. A qualified OCIO
config must resolve that exact id as a non-data scene-linear color space before any operation that
needs an OCIO transform; a matching alias, role, or display name is insufficient.

An escaping color intent retains immutable ownership of its working-space and configuration-URI
strings. `captureEvaluationColorIntent` preserves the aggregate request API, reuses an exact existing
owner, and reports allocation failure explicitly; ownership does not participate in semantic
equality. CPU/GPU evaluation, prepared-scene construction and asynchronous preview submission
capture raw caller views before publication. Preview settings, queued work, RAM/background keys,
thumbnails and retained frame identities keep that owner across color changes and New/Open.
Failed capture rejects the request; it never substitutes the Neutral configuration. Immediate UI
reads may remain borrowed while the session is live; cancelled workers still retain their captured
strings until they actually finish.

The compatibility spelling `EvaluationColorIntent::LinearRec709Scene` still denotes the zero-revision
`lin_rec709_scene` intent, preserving existing process-frame and reference-display bytes. Generalized
intents add their working-space id and config revision to cache/output identity. CPU image primitive
semantics version `7`, CPU evaluator semantics version `9`, and reference display-mapper semantics
version `2` remain the relevant primitive contracts.

Binding rules:

1. Import establishes explicit source interpretation before color-sensitive processing.
2. Scene/process pixels and display-referred presentation remain separate.
3. Compositing and light-like operations run in a declared scene-linear working space unless the
   node explicitly defines another model.
4. Perceptual operations declare their model and processing space; the implementation does not
   guess one from storage values.
5. Masks and data bypass color transforms by semantic role, while their numeric filtering behavior
   remains explicit.
6. Alpha association conversions are explicit. Zero-alpha unpremultiplication and RGB
   reconstruction use operation-defined behavior and diagnostics.
7. Negative and HDR RGB survive by default. Clamp, gamut mapping, and tone mapping occur only
   through operations whose declared purpose includes them.
8. Process-cache identity includes only configuration, context, process space, alpha, quality,
   precision, provider, and algorithm inputs that can change process pixels. Display-cache identity
   begins with the exact process-frame identity and additionally includes display/view, looks,
   monitor/output intent, prepared-processor identity, packing, and display algorithm versions.
9. CPU and GPU providers implement the same declared operation space and alpha equation. A backend
   cannot omit color processing or substitute display-referred math.

The implemented Solid authoring-to-process policy freezes its declared authoring encoding, identity
conversion, Float64-to-Float32 rounding, range failure, premultiplication order, and zero-alpha
behavior in tests. Layer opacity multiplies all four premultiplied components. Reference
source-over is:

```text
out.rgb = source.rgb + destination.rgb * (1 - source.alpha)
out.a   = source.a   + destination.a   * (1 - source.alpha)
```

RGB is not clamped, valid alpha remains in `[0, 1]`, and transparent borders are exact zero. A
future Color Mix or grade operation defines its own space and alpha contract; generic scalar Mix
does not define color interpolation.

### Multiply and Invert image effects

The `bloom.multiply` and `bloom.invert` Color nodes use the composition's declared scene-linear
working RGB. A canonical process pixel is premultiplied, so both effects first recover straight RGB
for nonzero alpha, apply their operation independently to each color channel, then multiply by the
original alpha. Alpha and the input image's display/data windows are unchanged. Zero-alpha pixels
remain canonical transparent black; hidden straight RGB is not reconstructed. Neither effect
changes a mask, alpha, or display encoding.

Multiply uses an animatable finite scalar `factor` with neutral value `1`: `rgb' = rgb * factor`.
Invert's initial color mode has no parameter: `rgb' = 1 - rgb`. Both accept negative and HDR RGB
and never clamp them. Thus an input channel of `2` becomes `-1` after Invert, while `-0.25`
becomes `1.25`; a negative Multiply factor may reverse signs. A result that cannot be represented
as finite Float32 produces a typed diagnostic and a pass-through rather than a saturated or
silently clamped pixel.
The sampled factor, operation kind, and input image identity participate in the process cache key;
the same math applies to preview and final routes. Separate alpha and mask inversion modes require
their own typed contracts and are not implied by this color mode.

## Viewer Analysis Requests

`EvaluationRequest`, `ProcessFrameIdentity`, and `PreviewRequestIdentity` carry an optional `roi`:
a nonempty integer pixel rectangle at the resolved request resolution, inside its display window.
The viewer stores its selection in composition coordinates and rounds its scaled minimum down and
maximum up when resolving a proxy request. Export requests leave ROI absent.

ROI limits raster output, not content geometry. Solid, Text, Shape, Layer Output, and Merge clip
applicable data windows before their row passes; evaluated bounds, polygons, anchors, and the
composition display window remain unchanged. A gather cannot crop its inputs to the output ROI:
transforms and their upstream dependencies currently retain conservative full input support so
translation, filtering, nested merges, and parenting cannot lose contributing pixels. Media decode
also retains its complete source. This is a correctness fallback, not a claim that every upstream
operation costs one pixel for a one-pixel request. Composition Output stores only the requested
rectangle. Both display preparers pad the remaining viewport with transparent black.

ROI participates in preview-frame keys and process-frame equality. In the operation cache it
participates only where it changes the result: an operation clipped to the ROI, and Composition
Output. Those ROI-dependent results are recomputed for every request and never retained, because
they recur only for the identical rectangle; a pixel probe moving across the Viewer would otherwise
add one set of entries per pixel (issue #414). A full-support operation (a layer's content, the
input of a gather, media) produces the same image with and without ROI, so it keeps the full-frame
key: every ROI request, and the full-frame request, share one entry per resolution. The plan cache
is unchanged. An absent ROI retains the existing operation-key encoding and raster path; primitive,
evaluator, animation, and display semantics versions are unchanged. Identity fixtures compare ROI
pixels bit for bit with full evaluation, retain the same bounds, restore the unchanged full-frame
result after clearing ROI, and show that a further ROI retains no new bytes.

`ViewAdjust{exposure, gamma}` belongs to display preparation and preview display identity. Exposure
multiplies linear display light by `2^exposure` after the view transform and before sRGB encoding.
Gamma applies `pow(encoded, 1/gamma)` to the encoded RGB before RGBA8 quantization. Alpha is
unchanged. The qualified path uses the processor's floating display output before quantization,
undoing only the sRGB encoding for the exposure step; it never adjusts already quantized bytes.
Finite EV values in `[-32, 32]` and gamma in `[0.01, 10]` are accepted. EV zero and gamma one bypass
adjustment and preserve both original display paths bit for bit.

Each viewer prepares its adjusted result on a cancellable worker, coalesces changed controls, and
rejects retired results. Its controls persist by editor-area identity in QSettings. The shared
neutral preview, thumbnails, process identities, authored colors, and export requests do not
inherit this adjustment. Adjusted and ROI-padded qualified buffers retain qualified provenance;
`PreparedPreviewFrame::displayBufferView()` selects the final viewport buffer.

The pixel probe reads straight RGBA8 and its normalized values from that viewport buffer. Reference
RGBA is the exact premultiplied `lin_rec709_scene` Float32 process value, widened to Float64 without
a color transform. A retained process image supports a constant-time pixel read. Display-only
cache hits, or positions outside the preview ROI, request a one-pixel ROI through the evaluator on
a worker. The probe caches one frame/position result, cancels superseded work, and discards late
results after leave or frame changes. Its evaluation reuses the operation cache's full-support
entries, so the first probe at a resolution pays for each layer's resampled content once and later
probes at any pixel of that resolution pay only for the one-pixel layer, merge and output steps. Exposure, gamma, channel display, and background never change
its reference values. Allocation limits, progress, diagnostics, and shutdown use the existing task
and evaluation contracts.

## Execution Classification

Every image or domain primitive declares the minimum information needed for correct scheduling:

| Class | Required contract |
| --- | --- |
| Uniform | one result independent of image coordinates |
| Pointwise | each output element depends only on corresponding inputs |
| Neighborhood | finite input halo derived from parameters and quality |
| Warp/gather | output-to-input coordinate mapping, filter footprint, and border mode |
| Reduction | input domain, deterministic combination policy, and result scope |
| Global | full-domain dependency and bounded resource strategy |
| Temporal | exact requested time, dependency window, sampling, and history policy |
| Analysis | cancellable work producing durable or derived data, diagnostics, and uncertainty |

Region of interest, domain of definition, halo, cancellation cadence, deterministic ordering, cache
identity, device capability, and fallback are semantic contracts rather than backend hints.

## Lowering, Diagnostics, And Compatibility

- Primitive IDs and node type IDs are stable, namespaced, and independently versioned.
- Node schema versions govern saved authoring compatibility. Primitive semantics versions govern
  compiled/evaluated behavior and cache identity.
- Compilation validates exact type, version, roles, and ports, then lowers only reachable nodes.
  Primitive selection happens during lowering, never through per-pixel lookup or type erasure.
- Published plans cannot contain invalid arity. If one does, it is an internal compiler/evaluator
  diagnostic. Numeric failures are scoped to the responsible node and operation.
- A node with no compatible lowering or provider is unsupported with structured diagnostics; it is
  not approximated silently. Preview may retain last-good pixels, while final output fails loudly.
- Primitive implementations avoid hidden allocation, process-global mutable state, ambient color
  configuration, implicit threading, and UI dependencies.
- CPU reference fixtures cover signed zero, subnormals, rounding environment, overflow, extreme
  finite ranges, negative/HDR color, alpha endpoints, hostile image extents, and cancellation.
- GPU/provider registration is per operation and precision. A provider that cannot meet the
  operation's fused, signed-zero, subnormal, failure, and tolerance rules falls back explicitly.

The planned artist-facing vocabulary is maintained in
[`../product/node-catalogue.md`](../product/node-catalogue.md).

## Linear and radial gradient sources

`bloom.linear-gradient` and `bloom.radial-gradient` version 2 are independent image-producing
authoring nodes. Each stores an animatable gradient value (1 to 64 straight RGBA stops; version 1's
start/end colour pair opens as its two stops, project-format.md "Gradient Colour Pairs In Document
1.39") and start/end `Vec2d` coordinates, animatable width/height, and a static integer repeat selector (`0` Clamp, `1`
Repeat, `2` Mirror). New nodes use the composition extent. Linear starts at the left edge and ends
at the right edge; radial starts at the composition center and ends at the right edge. The source
has a local data window from `(0,0)` to its resolved extent and inherits the composition display
window and pixel aspect. Authored coordinates are full-resolution local pixel-edge coordinates;
sampling uses pixel centers. The proxy request maps centers back to author space using the exact
horizontal and vertical resolution scales, so controls and composition geometry do not drift.

Linear `t` is projection onto the start-to-end vector. Radial `t` is distance from start divided by
the start-to-end distance. Horizontal distances are multiplied by physical pixel aspect in both
formulas. A zero-length vector chooses the first stop's color. Clamp confines `t` to `[0,1]`,
Repeat uses its fractional part, and Mirror folds over period two. The gradient is sampled at the
frame (a keyed gradient through `sampleGradientCurve`), each stop a value node drives then takes the
driven colour or position from the frame's value graph (#543, `CompiledGradientParameter::drivers`,
project-format.md "Gradient Stop Links In Document 1.40"), and its stops are sorted by position,
ties kept in stored order. Up to the first stop the first color holds, from the last stop the last color
holds, and between them the phase finds the last stop at or before it and its successor; two stops
at one position make a hard edge. Each stop carries the Float32 reciprocal of its span to the next
(zero for an empty span), and the local factor is `(phase - position) * inverseSpan` in Float32.
The two stops' colors interpolate in the effective scene-linear working space as straight RGB and
independent alpha; the result is premultiplied once for the process image. The phase is reduced in
binary64 and rounded once to Float32 (Mirror folds that Float32 value); alpha is then interpolated
in Float32 as two rounded products and one sum without fused multiply-add, so the CPU reference and
native GradientV1 agree bit-exactly on alpha. A two-stop table at 0 and 1 has a local factor equal
to the phase, so it reproduces version 1's start/end pixels bit for bit. RGB interpolates in binary64 within the 2e-6 parity bound. Negative and HDR RGB are retained when the resulting
Float32 process pixel is finite. The fractional right/bottom extent coverage scales the final
premultiplied pixel. ROI clips the requested output while preserving full source coordinates and
the composition display window.

The CPU evaluator shades rows with the shared cancellation and pixel budget gate. The GPU scene
preparer resolves the same animation operands, driven stops and windows and emits a semantic key
containing every sorted stop (position, reciprocal span, straight color), endpoints, repeat mode, scales, extent, pixel aspect, and the pinned GradientV1
shader digest. GradientV1 reads the stop table from a storage-buffer binding (two `vec4` per stop:
position and reciprocal span, then the straight color; the stop count in its push constants) and
refuses an empty, oversized, unsorted or non-finite table as Unsupported. The native operation
batch executes the gradient compute shader into a resident RGBA32F image, including single-generator batches; no full-frame CPU upload stands in for that
shader. Its normal completion uses the same fence proof, cancellation, image budget, and retirement
rules as other native batch operations. Native unavailability is reported explicitly and the
portable CPU reference remains the supported fallback.

### Gradient Interpolation And Colour Modes

A gradient value carries an interpolation, a colour mode and a hue direction (#546,
`document::GradientModes`, `gradient_interpolation.hpp`); Linear in RGB is the blend above and
keeps its exact operations. The established colour-ramp definition (`BKE_colorband_evaluate`) is
the reference:

- **Constant** holds the earlier stop's colour up to the next stop. **Ease** shapes the local
  factor as `3u^2 - 2u^3`. **Exponential**, which the reference's Color Ramp lacks, is the
  exponential easing of its F-Curves in their automatic (ease in) direction, the one Bloom's
  Exponential keyframe easing applies: `(2^(10(u - 1)) - 2^-10) / (1 - 2^-10)`, exactly 0 and 1 at
  the stops. These three hold the end colours outside the first and last stop like Linear.
- **Cardinal** and **B-Spline** weigh the segment's two stops and their neighbours with the
  reference's `key_curve_position_weights` (cardinal tension 0.71; the uniform cubic B-spline),
  repeating the first and last stop past the ends and extending the outer spans to 0 and 1, so they
  do not hold outside the end stops. The reference clamps all four channels to `[0,1]`; Bloom's
  stops are scene-linear and may be HDR, so only alpha is clamped and Cardinal may overshoot.
- **HSV** and **HSL** convert both stops with the reference's `rgb_to_hsv` / `rgb_to_hsl` (HSL's
  lightness is capped at 1, as the reference's is; a zero saturation divisor, which only colours
  outside `[0,1]` reach, gives no saturation), blend hue by the hue direction (`colorband_hue_interp`:
  **Near** the shorter way round, **Far** the longer way and a full turn between equal hues,
  **Clockwise** with the hue rising towards yellow and green, **Counter-clockwise** with it falling)
  and saturation, value or lightness and alpha linearly, and convert back. As in the reference,
  an HSV or HSL blend is always linear between neighbouring stops: the interpolation shapes RGB
  only, and the hue direction HSV and HSL only, each kept while the other is in use.
- A blended alpha is clamped to `[0,1]`.

The CPU reference evaluates every mode other than Linear RGB in binary32 through
`gradient_math::sample`, each product and sum a separately rounded operation and 2^x a degree-9
Horner polynomial of fused multiply-adds, and native GradientV1 and the Colorize program run the
same operations from `render::kGradientModesGlsl` (the build checks that gradient.comp carries that
exact text), within the 2e-6 process gate; the document's binary64 evaluation of the same
definition draws the editor's bar and Glow's octave tints. The modes as the blend reads them
(`effectiveGradientModes`: no hue direction in RGB, Linear in HSV and HSL) are GradientV1's push
constant words and part of the CPU and GPU semantic keys, so a mode the blend ignores never misses
a cache; the spline modes also carry the Float32 reciprocals of the spans from 0 to the first stop
and from the last stop to 1 in the first stop's spare table pair. A keyed gradient takes the
earlier key's modes between two keys. The Required coverage axes are
`feature.gradient.interpolation` and `feature.gradient.color_mode`.

## Image Effect Lowering

COLOR-3 advances compiled-plan semantics to **8** and CPU evaluator semantics to **9**.
Animation semantics remains **2** and image primitive semantics remains **7**. Existing pixel
fixtures are unchanged; version-bearing identity digests are independently re-derived. ADR 0024
later advances plan semantics to **9** and the evaluator was already at **10**; the output identity
goldens were re-derived with the oracle in `color5-identity-oracle.md`.

`CompiledImageEffect` contains its source node identity, an earlier input `OperationIndex`, a
closed kernel variant, and bypass state. Color-effect bounds follow their input; canvas effects
derive new windows and bounds from their sampled margins. Time dependence follows the input and
any animated or driven margin. The
initial identity kernel shares immutable input storage, including vector coverage when bypassed.
Color kernels use the straight-RGB alpha rule in
[`layer-graph-model.md`](layer-graph-model.md#image-effects). Future effects add kernels to this
common operation rather than introducing another source kind.

The kernel variant contains `IdentityImageKernel`, `CstKernel{fromId, toId}` and
`FileTransformKernel{lutAssetId, interpolation, direction, processSpaceId}` with an immutable
captured asset record. File identity includes its exact content digest. A bounded evaluator-owned
cache shares prepared CST processors by config revision/from/to and file processors by those
values plus file digest, format, interpolation and direction. Preparation is task work. File execution
uses cancellable chunks, and diagnostics survive warm frame memo hits. The process-space wrapper
is skipped for a proven identity LUT to retain bit identity. Canonical identity `.cube` tables
with the default unit domain are exact no-ops, including negative and HDR input.

`PointwiseImageKernel` adds scene-linear affine RGB processing. Exposure samples an animatable
amount in stops and scales straight RGB by `2^stops`; Offset samples an animatable amount and adds
it to each straight RGB channel. Both preserve alpha and the input data/display windows, leave
negative and HDR RGB unclamped, and turn transparent black into transparent black. A zero amount
is an exact identity. The sampled value and binary32 affine coefficients enter the effect cache
identity. The CPU reference uses cancellable row chunks; GPU preparation extracts an OCIO
`MatrixTransform` from the same coefficients and emits a resident `ProcessEffect` command through
the existing bounded scene executor. A preparation failure takes the explicit CPU fallback.

`CanvasImageKernel` adds Crop and Pad with nonnegative, animatable left/top/right/bottom margins
in full-resolution pixels. Each margin is rounded independently to the nearest output-grid pixel
for proxy evaluation. Crop removes those rows and columns from both data and display windows;
Pad expands both windows and fills the new data border with transparent black. Pixel aspect is
unchanged. A Crop that would empty either window fails with an explicit diagnostic;
invalid GPU preparation reports Unsupported and selects the CPU reference path.
No resampling or color conversion occurs: unchanged premultiplied pixels, including negative/HDR
RGB and partial alpha, are copied exactly. The CPU copies in cancellable rows and 4096-pixel
chunks; the production GPU scene emits a resident TranslationOpacity pass with integer sampling
coordinates and an explicit output display window. Both preview and final routes use the same
scene executor. Cache identity includes mode, sampled margins, input content, data/display windows,
pixel aspect, resolution, and the GPU shader artifact. The output uses ordinary pixel-budget
admission and retired resident-image ownership.

Checker is an image source with two animatable straight scene-linear RGBA colors, integer cell
width/height, and integer authored canvas width/height. It classifies a fixed 4×4 grid of samples
per output pixel in authored coordinates, using the full composition-to-proxy ratio; samples
outside the authored canvas are transparent. Its CPU reference and resident CheckerV1 compute
shader use the same integer-rational positions. Source-local data starts at (0,0), while the
display window and pixel aspect follow the composition. The GPU preparation requires each source,
composition and proxy dimension to be at most 16,384 because the shader uses bounded unsigned
32-bit products; larger valid documents receive an explicit Unsupported/CPU fallback diagnostic.
The single shader dispatch has constant work per pixel regardless of cell frequency. Resolved
colors, dimensions, windows and shader digest participate in cache identity.

Multiply Alpha consumes a base image and a matte at matching global pixel coordinates. It reads
the selected Alpha, straight Red/Green/Blue, or Luma matte channel, treats absent matte pixels as
zero coverage, reads Luma as the Rec. 709 luma of the premultiplied matte colour (as ADR 0028
defines for track mattes, so a feathered white matte edge gives its alpha rather than full
coverage), and multiplies the base premultiplied RGBA by the clamped coverage and animatable
unit factor. The amount is one Float32 product of the coverage and the factor, and each
premultiplied component is multiplied by that amount and rounded once more
(`render::multiplyAlphaAmount`), so the CPU reference and the resident shader produce bit-exact
alpha for every factor, not only a unit one. Zero-alpha matte RGB contributes zero; negative and HDR
matte channels are clamped only after extraction. The base image retains its data/display windows and pixel aspect. CPU and
resident two-image GPU paths share the coverage contract, with normal cancellation and budget
admission. The factor, channel and both input content identities invalidate cached output.

Set Alpha and Multiply Alpha share one closed matte selector, stored as the Integer
`channel` parameter in track-matte order: 0-4 select Alpha, Red, Green, Blue and Luma, and 5-9 the
same channels inverted (`render::MatteSelection`). Properties and node cards show it as a ten-entry
dropdown; a driver supplies the same stored value. An inverted matte uses the exact Float32
complement `1 - coverage` on both the CPU reference and the resident ImageCombineV1 shader (bit 3
of its channel word). The matte input is optional. A matte with no image at the requested time --
unconnected, an empty source, or a Layer outside its range -- is a transparent matte on every
route: zero coverage, so full coverage when inverted. The CPU reference reads no rows for it, and
GPU preparation emits the same combine dispatch with the base image bound in the matte slot and an
empty matte extent, so preview and final render never diverge or fall back for it. Required GPU
coverage features `feature.matte.invert` and `feature.matte.absent` carry those fixtures. Image
Mix's second input stays required, but a connected second image with no pixels at the requested
time is the same transparent image on both routes.

The plan also carries `bypass` and `look`; `EvaluationRequest::bypassLookNodes` skips only marked
nodes. Bypass shares upstream image storage and bounds. Active color transforms materialize the
RGBA32F image, transform straight RGB, then restore the original alpha association. Alpha zero
stays transparent black; failures never publish partially transformed rows.

Each effect has its own memo entry. Its key includes input content digest, kernel identity,
bypass, working-space id, and exact OCIO config revision. Ordinary input lifetime accounting,
cancellation, progress, and memory admission apply to the effect as to every image operation.

## Operation memoization

The session's `CpuCompositionEvaluator` owns a synchronized, byte-bounded LRU shared by its
foreground, RAM-preview and background requests. Solid, Text, image effects, post-transform Layer Output, Merge,
Composition Output and individual value-graph operations retain successful results. Image hits
share immutable pixel storage; they do not copy or rerender it. Absent trimmed-layer images can
also be retained. Failed value operations rerun so their diagnostics are reproduced. Cancelled or
failed image operations are never inserted; complete earlier operations remain reusable.

An operation address includes its plan revision and resolved content. The content key includes
project/composition and source-node identity, operation kind, its own resolved pixel-affecting
operands, composition format, resolution/proxy, and an optional ROI. Merge keys include input order
and whether a slot applies layer blending. Downstream keys include the SHA-256 content hashes of their inputs.
Exact time participates only for time-dependent operations. Values use their kernel options,
resolved operands and frame rate; these results are independent of image resolution. Keys encode
floating-point bits, including signed zero, without rounding or locale-dependent text.

A second lookup by resolved content adopts an unchanged operation into a new revision. Only the
latest revision address is retained, so editing an unrelated node neither discards sibling images
nor grows an alias table. An edited solid color invalidates that solid, its transformed Layer
Output, Merge and Composition Output; the text branch remains reusable. There is no revision-wide
operation-cache flush. Composition-format or proxy changes produce distinct image keys.

Compilation derives transitive time-dependence metadata for both programs. Curves and Time nodes
are dependent, as are operations reached through their value outputs, drivers or image inputs.
Static operations and a Merge of static inputs are invariant. Layer visibility additionally enters
its resolved key: crossing either half-open trim boundary cannot reuse the opposite visibility
state. This metadata is derived runtime state and does not change serialized plan identity inputs.

`playback/operation-cache-bytes` is the operation allocation from the session's one
`MemoryBudgetLedger`, which also allocates `playback/ram-preview-memory-bytes`.

Cache preferences are independent selected retention targets, not proportional weights in a
host-pressure policy. Nonzero operation and RAM-preview preferences are honored exactly. A missing,
invalid or zero value selects that pool's Automatic default; changing one preference does not
reduce the other, and decoded-video or other auxiliary pools do not subtract from a named target.

Automatic is usable memory and nothing else ([ADR 0037](../decisions/0037-memory-follows-the-working-set.md)):
measured physical capacity minus the RAM headroom preference (its default share of the capacity
until a preference is applied), or the historical 3 GiB fallback while capacity is unmeasured.
There is no fixed fraction of physical or available memory and no operation/preview split: each
automatic pool may use all usable memory, and the pools compete inside it under the eviction
priority below (finished preview frames are evicted last). An observed OS limit still applies.
Physical capacity is resolved once from the first successful worker delivery; a headroom change
re-resolves the total, and free-memory fluctuations do not.

These defaults are optional starting values, not physical-capacity ceilings on explicit preferences.
Selected targets are lazy retention limits, not preallocations or allocation guarantees. Preferences
Apply updates both live owners immediately without restart or rewriting the selected values.

Production calls `MemoryBudgetLedger::beginProcessSampling()` before constructing cache owners.
Reusable retention starts at zero until a complete usable worker sample establishes process-limit
readiness. This is initial readiness, not a zero-byte OS limit. Foreground processing uses separate
phase admission. Construction and settings getters never probe the OS.

The application owns one `MemoryBudgetController`, with at most one `BlockingIo` sampling task,
no sampling backlog and a five-second cadence. Task completion delivers host/process observations
independently of Jobs monitoring. Between those samples a `MemoryPressureMonitor` thread reads the
machine sample every 250 ms (Linux also wakes on a `/proc/pressure/memory` PSI trigger). Effective
available memory is available memory minus the process's own swapped memory (Linux `VmSwap`, macOS
compressed task memory, Windows private bytes outside the working set). A sample below the RAM
headroom is queued to the controller's thread and applied with
`MemoryBudgetLedger::applyPressureSample`, so reusable pools shrink by their share of the deficit
to headroom plus the hysteresis margin (`memoryPressureMarginBytes`, an eighth of the headroom, at
least 256 MiB) within one interval, each pool evicting its least recently used entries; they grow
only above headroom plus margin. Only a severe stall counts as pressure, and only while available
memory is inside headroom plus margin: Linux PSI "full" stall time of at least 10% of the interval,
the macOS VM pressure level (warning or critical), the Windows low-memory resource notification.
Light "some" stalls, swap occupancy and swap traffic never shrink pools or block growth (stale zram
swap and steady low stalls are normal while memory is plentiful). Eviction is deficit-only; the
user purge is the only full clear and also trims the heap (`requestHostTrim`). The ledger counts
the reusable bytes each application released; the monitor then calls `releaseFreedHostMemory()` (glibc `malloc_trim`), and
`configureHostMemoryReturn()` fixes glibc's mmap threshold at 1 MiB when sampling starts, so freed
pixel buffers are unmapped rather than left resident in heap arenas. macOS and Windows allocators
already return large blocks on free.

Evict-on-demand: a `GpuProcessingPhasePolicy::reserve` that returns `AwaitingCapacity` on the host
domain calls its retention reclaimers with the shortfall (need minus free) and records it
(`lastAwaitingNeed`). The application registers `MemoryBudgetController::workReclaimer()`, which
queues `MemoryBudgetLedger::reclaim(shortfall)` (at least 256 MiB, coalesced) to the controller's
thread and requests a host sample at once (the sampler trims the heap first); headless export
providers reclaim synchronously on the process ledger and force their next sample. Reusable pools
see `poolAvailableLocked`: available memory minus own swap, bounded by the room left in each finite
cgroup domain (limit minus unreclaimable usage), and never grow into `setActiveWorkClaims` (the
policy's pending host claims, `UsableCapacity::hostPendingClaimBytes`, applied after each sample).
Pending claims are the claims not yet backed by a rooted host allocation: allocated work and retained
packed preview frames are already in the measured usage, and counting them a second time froze
every pool while a RAM preview filled, so the preview evicted its own frames with memory still
free. An export waiting for capacity reports
`ExportDeviceRoute::WaitingForViewer` only while `viewerPriority()` shows the Viewer waiting or
active; otherwise `WaitingForMemory` with `ExportJobProgress::memoryNeededBytes`. Cache callbacks apply on the owner thread; status bars and
Preferences observe cached state. Shutdown cancels and stops admission without waiting on a held
worker. A preference change requests a new sample but applies retention targets immediately.

One function wires this for a Qt composition root: `ui::connectApplicationMemoryBudget` (with
`ui::applicationMemorySampler`) delivers each sample to the processing policy and the policy's host
claims to `setActiveWorkClaims`, registers the work reclaimer, and applies the RAM/VRAM headroom
(the saved preference, else 10% of measured capacity) to the policy and to
`MemoryBudgetLedger::setRamHeadroom`, again whenever a sample changes the computed default. The
application and the packed RAM-fill benchmark harness both call it. A ledger without a headroom
has no usable-capacity model: under a finite OS limit it divides the whole absolute limit between
the reusable pools (see below), which leaves no room for work. That is what the harness ran before
#411: inside a 20 GiB cgroup the operation cache held 11.4 GB, the preview cache up to 7.6 GB and
the other pools about 2.3 GB, the process sat at the limit with under 1 GB outside the pools, and
2160p video fills stopped at 182–207 of 240 frames waiting for capacity no cache gave back.

#### What the pools count

Every reusable pool reports a `MemoryPoolHolding` to the ledger (#416):

- `hostBytes`: host bytes the pool retains and can give back by evicting;
- `hostPendingRetirementBytes`: host bytes already evicted that stay allocated until a retirement
  worker frees them after the GPU work that reads them;
- `deviceBytes`: device-resident bytes.

Budgets, deficit and reclaim shares, and `yieldToPreview` grants use `hostBytes` only. Host totals
(`reusableRetainedBytes`, `MemoryBudgetState::retainedBytes`, the bytes released for a heap trim)
are `hostBytes` plus pending retirement, so evicted bytes are neither lost before they are freed
nor reclaimed a second time. Device bytes never reach a host budget; `reusableHolding()` reports
them for attribution next to the device claims of the processing policy. A pool registered with a
plain byte callback reports host bytes only.

What each pool counts:

- `OperationCache` charges each distinct allocation once, however many entries reference it, at
  its real size (`Rgba32fImage::pixelStorageByteCost`, the vector capacity, plus the image
  object; an encoded source revision's `retainedBytes`), and per entry only its metadata (the
  entry, its key and values). An identity effect and a composition output often publish the
  same image as another entry; before #416 each entry charged the full pixels,
  so the pool reported more than it held. `OperationCache::memory()` splits the total by kind,
  reports the pending part, and reports `sharedReferenceBytes`: the extra references per-entry
  accounting used to add. Shared storage counts under the kind that stored it first. Evicted
  entries of a pool bound to a host retirement owner stay charged as pending until their worker
  frees them; a store of the same image meanwhile takes it back out of pending without charging
  it again. The cache's budget bounds `hostBytes`, as before. Nothing in it is device memory, and
  nothing in it is a host copy of an upload: staging is scratch
  ([ADR 0037](../decisions/0037-memory-follows-the-working-set.md)).
- `PreviewFrameCache` counts CPU and packed frames as host bytes. A GPU-resident frame is device
  memory: once `setGpuResidentLimits` gives the cache a device budget, resident frames count as
  `deviceBytes`, only the GPU-resident limits evict them, and they no longer take room from the
  host byte budget or from the Ordinary pools through `yieldToPreview`. A standalone cache without
  a device budget charges resident frames to its byte budget, as before.
- The decoded video cache, the media disk cache write queue and the asset proxy and decoded-audio
  pools are host-only and report plain host bytes.

No pool counts allocator overhead, freed memory the allocator keeps in its arenas, library and
driver mappings, or memory held by work in flight; these are the measured remainder between the
host totals and anonymous RSS. The packed RAM-fill benchmark harness reports the pool split
(`memoryAtPeak` and `BENCH_MEMORY`: `operationPendingBytes`, `operationSharedBytes`,
`reusablePendingBytes`, `reusableDeviceBytes`, `deviceClaimBytes`), and `tools/bench/run_bench.py` samples the process tree's device memory
(`peakDeviceBytes`, from `nvidia-smi --query-compute-apps` where available).

#### Eviction priority during RAM Preview

Outside a RAM Preview fill every reusable pool yields in proportion to what it holds, least
recently used entries first within the pool. While a fill is active, finished preview frames come
first (owner decision, #413): `RamPreviewController` holds a `MemoryBudgetLedger::PreviewFill`
scope from `start()` until `finish()` (completion, cancel, failure or shutdown), and each pool is
registered with a `MemoryEvictionPriority`. `PreviewFrameCache` is `FinishedPreview`; the
operation cache (operation intermediates, decoded media and source revisions), the decoded
video cache, the media disk cache write queue and the asset proxy and decoded-audio pools are
`Ordinary`. `memoryEvictionShares` is the policy:

- a budget deficit below the headroom and a work reclaim (`MemoryBudgetLedger::reclaim`) are
  covered by the `Ordinary` pools first, in proportion to what each holds. Finished preview frames
  give only the remainder, so the frame being prepared is never starved by the frames before it. A
  reclaim leaves the budget of a pool asked for nothing unchanged during a fill;
- a preview cache that has no room for a finished frame asks `MemoryBudgetLedger::yieldToPreview`
  before it evicts one of its own. The `Ordinary` pools evict up to the frame's shortfall and the
  preview budget rises by what they evicted (`PreviewFrameCache::Statistics::
  previewFirstYieldedBytes`). A pool rooted on a host retirement owner frees evicted entries
  after their GPU work, so its retained bytes fall a little later; until the next budget application a pool
  counts as holding at most the budget it was last given, and the same bytes are never granted
  twice. The exchange is memory-neutral: the preview pool never takes free
  memory or the headroom and pending work claims that the next frame needs this way. It applies
  only with a RAM headroom (usable-capacity retention); explicit per-pool budgets without a
  headroom stay exact;
- free memory beyond headroom plus margin and pending claims is left to the work in flight and
  the finished frames it becomes: during a fill only `FinishedPreview` pools grow into it, while
  `Ordinary` pools hold what they have and cycle their own least recently used entries. Outside a
  fill every reusable pool takes an equal share, as before. Before this, a 2160p fill let the
  operation cache take most of the free memory early (prepared upload host copies, 7–8 GB, which
  a linear fill never reused and which ADR 0037 has since removed), which also narrowed the frames
  in flight that free capacity sizes.

When the `Ordinary` pools hold nothing more, the preview cache evicts its own least recently used
frames and the fill ends `PartialBudget`. Ending the last fill scope restores proportional yielding.
The application and the packed RAM-fill benchmark harness share this through the same
`RamPreviewController` and `PreviewFrameCache`.

### Actual process constraints and active work

An observed finite OS process/container limit can constrain total reusable retention. Its reusable
ceiling is the smallest **absolute limit** among visible domains. It is not remaining free memory,
and no invented reserve is subtracted. Only when the registered reusable targets together exceed
that absolute limit do those owners divide it proportionally. Selected settings remain unchanged;
the UI names the combined OS constraint and shows each owner's actual allowance. Auxiliary pools
participate in this actual shared constraint; they do not reduce named budgets on an unconstrained
process. `LiveWork` owners are separate from reusable retention.

For active processing, the shared phase policy checks measured available host memory and each
finite domain's paired `max(0, limit - current usage)`, capped by reported physical capacity when
known. It does not apply an additional percentage, reserve, cache-budget fraction, or growth delay.
The delivered request and aggregate capacities describe active work independently of selected cache
budgets. Measured zero refuses growth; unknown, incomplete or stale observations cannot authorize
new growth. Held leases, request counts, device limits, cancellation and retirement remain charged.
Cache counters are not unique physical backing and receive zero reclaimable-memory credit.

The parser follows actual Linux cgroup v2 membership and compatible mounts, preserving visible
ancestors with a bounded 32-domain walk. A finite domain with missing usage may tighten the absolute
retention ceiling, but cannot authorize active-work growth. Partial, stale, failed and unsupported
samples cannot expand a previously observed finite constraint. Fresh, monotonic, complete paired
samples permit immediate recovery to their measured constraint; there is no half-cap seed,
ten-second hold, percentage retention ladder, or incremental recovery policy.

Removal of an observed constraint requires complete data from the same verified membership,
selected mount/filesystem and cgroup/mount namespace view, with explicit `max` and valid current
usage for every remembered finite domain. The worker verifies the view before and after scalar
reads. Missing, malformed, changed-view or stale observations retain the constraint, as does
failure to retain its bounded history. Initial verified hierarchy-root absence may establish
host-only operation but cannot omit a remembered domain from removal proof. Hidden limits are not
proved absent. Linux memory-v1 and Windows/macOS process limits remain explicitly Unsupported;
without an observed finite constraint, they use disclosed host-only best effort. Native
Windows/macOS qualification remains pending.

These checks are admission and retention accounting, not a whole-process OOM guarantee. Shared
aliases, allocator overhead, scratch, driver allocations and other live products still require
allocation-owned accounting. The existing GPU owner partition and final-render request defaults
have separate restrictions recorded in the user-control audit; this change does not claim to have
removed every device or export policy.

### Retention, diagnostics and lifetime

There is no host-pressure cache-shrinking policy. Low free RAM and active swap growth remain
observable; neither silently changes a selected cache target. Swap growth is measured relative to
valid previous samples, not inferred from static swap occupancy. The UI reports increasing swap
usage without claiming caches were trimmed. A real finite process constraint reports
**Cache budget reduced by system memory limit**. The tooltip distinguishes selected totals, actual
operation/preview allowances, observed OS limits and available memory, retaining that explanation
while RAM-preview progress is shown.

Operation/decoded-media entries, display previews, decoded video, disk-write queues, proxy
thumbnails and decoded audio retain their own selected budgets. A lower actual allowance evicts
reusable entries; publication rechecks admission so an old result cannot refill beyond the current
limit. Eviction drops only the cache's references. Active decode, viewer/playback pins and pending
retirement retain ownership until they finish. Active disk writes retain their charge; callbacks
never perform disk I/O or wait for a writer.

`ExportResourceLedgerV1` admits production export claims through the shared processing host root.
Retained attempts, independent publication scratch, sequence queues and decoded audio expansion
remain charged through their owning work and retirement. The old 4 GiB concurrent and 2 GiB
per-job allowances apply only to unrooted compatibility callers; an explicit ledger service cap
still binds a rooted claim. A sequence caller's explicit queue cap checks its initial frame-queue
envelope without limiting later decoded audio. Sequence queue admission refreshes provider-owned capacity on its
worker before reserving, and later audio expansion refreshes again. The media provider's separate
frame geometry and index protocol limits are capability limits, not physical RAM estimates.

If retaining an entry throws `std::bad_alloc`, `OperationCache::store()` and
`PreviewFrameCache::insert()` unwind the partial insert, count the failure, and preserve the produced
value. Caching remains an optimization; failure to retain must not invalidate a successfully
rendered frame.

### Cache byte accounting

Every cache counts retained bytes against a configured ceiling and a live admission budget. The
status bar shows the operation/preview accounts; other counters support tests and diagnostics.

| Pool | Bound | Counter |
| --- | --- | --- |
| `OperationCache`, operation entries | operation budget | `retainedBytes(Operation)` |
| `OperationCache`, decoded media entries | operation budget (shared) | `retainedBytes(DecodedMedia)` |
| `PreviewFrameCache` display buffers | preview budget | `residentBytes()` |
| `DecodedVideoCache` frames | weighted video allowance | `residentBytes()` |
| `MediaDiskCache` pending async writes | 64 entries **and** up to 256 MiB, scaled by ledger | `statistics().asyncQueueBytes`, `peakAsyncQueueBytes` |
| `AssetController` proxy thumbnails | 512 entries **and** up to 8 MiB, scaled by ledger | `proxyCacheBytes()` |
| `AssetController` decoded audio buffers | up to 2 GiB aggregate, scaled by ledger | `decodedAudioBytes()` |
| `ExportResourceLedgerV1` export frame queue | sampled processing-root headroom, plus any explicit caller cap; legacy unrooted 4 GiB service/2 GiB job defaults | `chargedBytes()`, root reservations |
| Viewer display buffers | one retained frame plus at most one channel-remap copy, per open viewer | -- |

The disk cache's pending-write queue is the pool that mattered. It was bounded at 64 entries and
never at bytes, and a queued write owns the last reference to a decoded Float32 image: 64 pending
4K RGBA32F frames are 7.9 GiB and 64 pending 8K frames are 31.6 GiB of process memory no budget ever
saw. It is now bounded by bytes as well, and the byte bound is the binding one -- a write whose
payload alone exceeds the capacity is refused at `storeAsync()` rather than staged and then
discarded by the writer thread. A refused write is counted in `droppedAsyncWrites` and only means
the next read decodes again; it can never mean incorrect pixels.

No pool above is keyed by window size, screen or device pixel ratio. A resize, a move to another
screen or virtual desktop, a ratio change or a hide/show re-lays out and repaints the retained
Viewer frame. Under the Auto resolution setting the displayed scale (which includes the device
pixel ratio) can select another divisor; that frame is prepared once on the GPU route and then
served from the preview frame cache. The pixel probe then samples the newly shown frame: the
first probe at that resolution adds each layer's resampled content to the operation cache, and
later probes and later window changes add nothing. See
[Window, screen and pixel-ratio changes](gpu-backend.md#window-screen-and-pixel-ratio-changes)
for the soak that asserts this.

The shared cache accounts retained image/value storage and entry/key overhead, evicts by
least-recently-used last use, and refuses an entry larger than the budget without evicting useful
entries for it. Runtime `setByteBudget(0)` disables retention and releases entries. Images already
held by an active evaluation or published frame live until those owners release them; eviction does
not invalidate readers. Shared image storage is conservatively charged for every retaining entry.
Concurrent misses may independently evaluate the same operation; cache access is synchronized, but
row kernels never execute under its mutex.

Decoded media and immutable encoded still-source revisions are stored in the same evaluator-session
cache with revision-independent content addresses. Source revisions own the bytes used for SHA-256,
probe and decode, include encoded bytes and metadata in retention charges, and obey the explicit
interactive reuse/final revalidation contract in [Media I/O](media-io.md#disk-cache).
Because a burst of new revision-qualified operation entries otherwise makes cold media the oldest
entry, a decoded entry receives an eight-cache-access grace period. Unprotected operation entries
are evicted first during that grace; if every entry is protected, the byte budget still wins.
This small priority rule keeps a recently decoded source available across preview and export work
without making decoded media immortal.

The existing display-frame cache remains the upper level. A frame hit needs no evaluator work;
a miss consults operation memoization before running kernels. Only display buffers count against
the frame-cache budget; retained process images count against the separate operation budget.
`ProcessFrame::operationCacheStatistics()` exposes that request's hits, misses and evaluated node
IDs for later status-bar use, without introducing UI. Preflight validation, request pixel-storage
limits, cancellation, progress and row-band execution still apply on cache hits. Memoization
changes no pixels, identity goldens or evaluator/primitive semantics versions.

Interactive override plans and requests bypass both lookup and insertion, including value results.
Their temporary gesture values cannot populate the operation cache. `bypassOperationCache` also
provides a serial uncached reference for verification. Caching is process-local; nothing is persisted
in the project or on disk.

Evaluated operation geometry is retained beside cached display pixels. The frame-cache budget
counts both payloads; releasing the Float32 process image leaves bounds queries and overlays available
on playback cache hits without evaluation.

### Current Compiled Bounds Contract

Compiled Solid dimensions, Text layout operands and Shape geometry operands are required. Shape
bounds include the enabled fill and stroke. Layer transforms always place the
local-bounds anchor at the authored position, and Merge always unions its inputs' bounds. Plans
carry no historical evaluation selector. Unsupported document node versions are rejected before
compilation. Current identity uses plan semantics 9, animation sampling 2, evaluator 10, and render
primitives 7. SHAPE-1 adds shape pixels and TEXT-2 adds box-text pixels, both while preserving all
existing source pixel goldens.

## Transformed vector coverage

Image primitive semantics 7 adds affine path coverage and glyph outlines. Curves flatten with an
output-space error bound; stroke outlines are built before the affine map. Compound contours use
nonzero winding, and native text-box clipping is transformed with the glyphs. The original native
text coverage routine and its byte goldens remain unchanged. CPU evaluator semantics 8 selects
this coverage for transformed vector-only Layer Output chains and retains bilinear sampling at
raster boundaries.

## Text layout query

`render::layoutText(font, content, parameters, options)` returns a checked, Qt-free `TextLayout`:
line indices, half-open source UTF-8 byte ranges, text-space line boxes, glyph line/byte positions
and advance rectangles, plus caret height. It uses the same pen placements as native coverage and
transformed glyph outlines, including kerning, letter spacing, horizontal alignment, point anchors,
word wrapping, snapped line rows and vertical box alignment. Advance rectangles include the gap to
the next glyph's pen; their widths sum to the line advance. Empty content retains one empty line
and a caret at the text anchor. Explicit empty lines retain their source ranges.

Wrapping retains original byte positions even where its established whitespace normalization omits
source whitespace or inserts a soft break. Consumers convert source bytes to their input toolkit's
cursor units; Qt UTF-16 positions never enter the render API. The query returns logical advance
geometry without allocating a coverage bitmap, and shares validation and cancellation with the
placement path. It changes no rendering semantics, schema versions or existing pixel goldens.

### Video Source (MEDIA-3)

`bloom.video-source` version 1 lowers to `CompiledVideoSource`: source node, captured video asset,
start frame, loop mode and color-space override. It emits image and audio ports. Exact composition
time minus `startFrame / compositionRate` maps to the captured video rate. Negative time holds
frame zero; Hold clamps to the final frame, Loop wraps, and Ping Pong reverses at the endpoints.
The first video stream supplies pixels. The first audio stream supplies the existing clip model,
using the same start offset and Layer/Merge participation rules; audio beyond its buffer is silent.
Loop and Ping Pong currently apply to image sampling, while audio retains ordinary clip timing.

Evaluation runs the bounded worker session/cache path off the UI thread, then converts supported
Rec.709 or sRGB-tagged YUV to scene-linear premultiplied pixels. Unsupported color and changed
content produce diagnostics instead of cached stale pixels. Video source selection participates
in operation identity, including content digest, stream, frame and interpretation.

At the composition-source landing, compiled-plan semantics was **7**, animation **2**, evaluator **8**, primitives **7**;
existing arms' pixel semantics are unchanged. The independent `s5-identity-oracle.py` audit
reproduced all plan-6 analysis/output pins before deriving plan-7 pins, preserving the four
preimage lengths (PNG analysis 1922, EXR analysis 1485, PNG output 669, EXR output 567 bytes).

## Exact Flip And Rotate 90 Image Nodes

`bloom.flip` v1 and `bloom.rotate-90` v1 lower to the required `DiscreteImageKernel` image-effect
alternative. Flip has static Boolean horizontal (default true) and vertical (default false)
controls. Rotate 90 has one static signed integer `quarterTurns` control (default 1), normalized
modulo four; positive turns are clockwise. Generic AddNode/parameter commands own edits and undo;
the versioned definitions and parameter records use ordinary project persistence.

At Full resolution and equal-axis proxies these nodes permute the stored pixel lattice without
color conversion, unpremultiplication, filtering, alpha arithmetic or HDR clipping. Odd quarter
turns at unequal-axis proxy scales instead conjugate the rotation by those scales and use the
shared premultiplied bilinear affine sampler on CPU and GPU, preserving Full-reference authored
bounds and physical aspect. Flip reflects within the input data rectangle.
Rotation exchanges data width/height on odd turns and retains the data rectangle's top-left
origin on every turn, including negative and off-canvas origins. The display canvas and pixel
aspect ratio are retained: an image operation does not resize the composition. Full overscan is
retained for downstream transforms; Composition Output/ROI determines the final visible crop.
The CPU reference copies exact premultiplied RGBA32F samples with row/chunk cancellation,
progress and the existing checked allocation budget. Invalid output windows or failed allocation
fail evaluation rather than publishing an unchanged final image.

GPU preview and all final routes use the same production scene builder and resident affine
executor. A signed permutation matrix with integer translations produces zero fractional sample
weights, so the existing affine shader's explicit zero-factor branches select the exact source tap;
there is no host full-frame pixel preparation. The ordinary GPU capability, allocation, cancellation,
cache-key, live-pin and retirement contracts apply. CPU fallback remains explicit on unsupported
native devices/platforms, and for data origins beyond the exact Float64 integer-coordinate
domain (absolute coordinate above 2^52). Windows/macOS native qualification is pending, as for the shared affine
backend. Tests cover every turn, both axes, nonsquare off-canvas windows, signed/HDR/partial-alpha
sample preservation, exact affine sample bases/zero weights, production preparation, warm cache,
Properties command undo, and verified archive reopen. A separate native image-effect harness adds
an asymmetric stroked image fixture for both axes, their combination, and turns 1/2/3; it requires
exact CPU/GPU pixels, a real resident affine dispatch, no executor readback, and warm cache reuse.
That harness and the required coverage gate must run on a device before claiming native parity.

## Separable neighborhood filtering

Gaussian Blur and Box Blur share the CPU/GPU kernel, explicit data-window halo and edge policy
defined in [Image Filters](image-filters.md). Unlike pointwise color kernels, they filter associated
RGBA and may expand the data window. Matte Blur (#337) runs the same Gaussian kernel with the
Coverage channel policy over a Mask: alpha only, written to every channel. Matte Erode and Matte
Dilate (#338) run the same separable neighborhood over a Mask reduced by minimum or maximum.
Preview and final rendering use the same production scene path.

## Channel sets and Shuffle

ADR 0026 advances compiled-plan semantics to **10** and CPU evaluator semantics to **11**. Existing
plans evaluate unchanged; only their version-bearing identities change.

- *Channel algebra compiles away.* An Image source's `channels` output, Remove (shown as "Select
  Channels"), Channel Copy and a Shuffle's own `channels` output touch no pixels, so the compiler
  folds them into a `CompiledChannelExpression`: the referenced sources (asset, start frame, loop,
  part, input interpretation) and topologically ordered steps (Source, Remove by pattern with
  `keep`, Copy by pattern where a copied channel replaces a same-named one, and Rename into
  `<layer>.R/G/B/A`). Patterns are comma-separated names, layer names, `*`/`?` globs or `@role`
  tokens (`color`, `alpha`, `luminance`, `depth`, `normal`, `motion`, `position`, `uv`, `id`,
  `coverage`, `data`). An Image source whose color `image` output feeds nothing compiles no image
  operation.
- *Shuffle is the pixel operation.* `CompiledChannelShuffle` holds the expression, four slots (a
  resolved channel name or the constants `0` and `1`), a mode and `associated`. An empty slot (the
  default for red, green and blue since #487; alpha defaults to `1`) reads the input's default
  color layer: the root `R`/`G`/`B` (and `A`) when the root carries R, G and B, else the first layer
  in channel order that does (`runtime::defaultShuffleSlotChannel`), so a rootless multi-layer
  render shuffles its beauty pass. Without such a layer the slot is the warning "Shuffle input has
  no color layer for its default slots". The resolved channel enters the keys exactly as a named
  one. At evaluation the
  expression is resolved against the identity-verified probes (host parameter resolution shared by
  the CPU evaluator, GPU preparation, the Viewer listing and layered output). The gather reads only
  the named channels of each contributing source into one RGBA image in the anchor source's frame
  (the first slot reading a source): the data window is the union of the contributing channels'
  windows, and a sample outside a channel's own window is an explicit zero. The process contract
  then applies as for OpenEXR ingest: non-finite components become the node's `nonFinite` value
  (#480; 0 by default, for example a far depth), alpha is clamped, zero-alpha emission is dropped
  (the process image is premultiplied, so a straight slot under alpha 0 is transparent black), and
  every repaired pixel is counted.
- *Auto (#480).* The default mode of a new Shuffle or Pass. It is `Raw` when a mapped red, green or
  blue channel is a data channel (any role but colour, luminance, alpha and coverage, or a channel
  with Data encoding), else `Colour`. An explicit `Colour` mode over a data channel is refused with
  the warning "Shuffle Color mode would color-convert the data channel <name>; choose Raw or Auto"
  and no image, so a saved project with that mapping opens with the diagnostic instead of
  colour-converted data. CPU evaluator semantics advances to **14**. Keys add the effective mode and
  the non-finite value.
- *Color and Raw.* `Raw` places the mapped values in working-space RGB unchanged. `Colour` converts
  the RGB triple from the input color space of the red slot's source (resolved like an Image
  source, including Auto from the part's chromaticities) to the working space. `associated` says
  the mapped RGB is already premultiplied by the mapped alpha (OpenEXR); a Color conversion
  unpremultiplies before and premultiplies after it, and a straight mapping is premultiplied once.
- *GPU.* Host preparation produces the same gather without color conversion as a raw upload; the
  proxy PointResampleV1 gather and, for a non-identity Color mapping, the input-to-working OCIO
  `ProcessEffect` are GPU commands, exactly as for an Image source. The raw upload is cached under
  the gather identity (source decode identities, slots and `associated`); the CPU result key adds
  the color interpretation. The coverage gate carries Required Shuffle, Remove and Channel Copy
  fixtures with native parity.
- *Viewer inspection.* A Viewer inspection compiles a viewer-only plan that reaches only the
  inspected ChannelSet and outputs a Raw, associated, alpha-1 Shuffle of one channel (gray) or three
  (RGB). It never enters the composition's own plan, cache entries or any render or export.
- *Pass (#480).* `bloom.channel-pass` is a Shuffle whose slots follow its `pass` parameter: a
  channel name maps `{c, c, c, 1}`; a layer maps its `R`/`G`/`B`, `X`/`Y`/`Z` or `U`/`V`/`W` (at
  least two present; else its first three non-alpha channels) with `0` for a missing slot and `1`
  for alpha; empty reads the first layer holding a Data-encoded channel, else the first layer.
  `associated` is off. Over file channels the mapping is derived at evaluation, after resolution
  (`CompiledChannelShuffle::pass`); a pass the set lacks is the warning "The pass is not in the
  channel set: <name>".
- *Image to Channel Set (#480).* `bloom.image-to-channel-set` adds an image-backed
  `CompiledChannelSource` (`imageBacked`, the image operation, and the static descriptors
  `<layer>.<name>` for its stored red, green, blue and alpha lanes; Data encoding keeps a name's data
  role and makes colour roles Data, Color encoding gives the lanes colour and alpha roles). The
  compiler knows these channels without a probe (`detail::staticChannelSet`), so a Shuffle or Pass
  whose slots read only them and constants lowers to a `ChannelSelectKernel` over the first image a
  slot reads, then one `ChannelCombineKernel` per slot read from another image, with `premultiply`
  set for a straight mapping: exact stored-channel gathers with existing GPU coverage. A slot that
  mixes image and file channels, or that a Channel Copy of unknown file channels may replace, stays
  the file gather and is refused at evaluation with a diagnostic. A Video source's `channels`
  output is the same image-backed source over its decoded frame (`R`, `G`, `B`, `A`, colour in the
  working space). Cryptomatte refuses image-backed rank channels. After time scoping the compiler
  gives each image-backed source of the Composition Output's channel layers its own plan
  (`CompiledChannelSource::imagePlan`: the plan re-rooted at that image through a Merge, then
  pruned), and drops operations nothing reaches from the output any more (an image read only by
  such a layer, or by a Shuffle leaf that names its channels statically); a plan where every
  operation is reached compiles exactly as before. The export evaluates each layer plan on the
  frame's own route and reads it once at the codec boundary (`frame-output.md`).
- *Composition Source channels (#480).* A Composition Source's `channels` output is the precomp's
  layered render: an image-backed source for the composite (`R`, `G`, `B`, `A`, colour in the
  working space, over the Composition Source's own image operation, or one the compiler adds when
  only `channels` is used), then the nested Composition Output's channel expression inlined into
  the reading expression (its steps renumbered, a Remove keeping the nested output's Layers list
  when it is set) with the composite copied over its root `R`, `G`, `B` and `A`. A muted source is
  an empty composite. Nested file sources keep their asset and gain the Composition Source as the
  outermost entry of `CompiledChannelSource::nesting` (outermost first; entry 0 indexes the reading
  plan's nested plans and curves, entry k+1 those of the plan entry k names). Every route
  (`detail::resolveChannelSourceTimes`, shared by the CPU reference, GPU media preparation, the
  layered output and the channel listings) reads such a source at the mapped nested time: each
  level resolves its offset and scale on the clock of the plan that owns them (the reading
  operation's resolution for entry 0, a sample of the nested plan at its own time for deeper
  entries), applies `mapCompositionTime` (offset, scale, Hold/Loop/Ping-pong over the nested
  duration) and reads the file at the innermost time with that plan's frame rate. The source cache
  identity is therefore the frame actually read; an unrepresentable time is the warning "Composition
  source time cannot be represented" and a cancelled resolution cancels the read. Nested
  image-backed layers (Image to Channel Set, Video `channels`, a deeper composite) carry their
  `imagePlan`; the reading plan registers each such plan as a nested plan and reads it through a
  `CompiledCompositionSource` with the precomp's own time mapping, so the composite and those layers
  lower to Channel Select / Channel Combine over nested Composition Source images on every route
  (`feature.channels.composition_source`). A Shuffle mixing them with file channels is refused like
  any image/file mix ("Shuffle reads the image channel <name> with file channels; ..."). A default
  (empty) slot whose set holds a root `R`, `G` and `B` the compiler can name reads that root even
  when the set also holds file channels, as resolution would. Nesting cycles are refused when the
  plan compiles ("Composition nesting cycle"). Pruning a channel layer plan keeps the Layer Output
  of every time clock a kept operation uses and drops the clocks no kept operation uses. Plan and
  evaluator semantics versions are unchanged: a plan without a Composition Source `channels` read
  compiles and evaluates as before.

## Cryptomatte

Issue #364 adds `bloom.cryptomatte`, following the Cryptomatte specification (v1.2) as compositing
applications read it, so artists recognize it. Plan and evaluator semantics versions are unchanged:
no existing plan evaluates differently.

- *Node.* Inputs: `channels` (ChannelSet, required) and `image` (Image, optional). Outputs: `mask`,
  the ADR 0028 Mask transport (premultiplied white with the coverage in all four channels), and
  `image`, the input image times the coverage (matte in alpha) through the existing MultiplyAlpha
  combine, or the white matte itself without an input image. There is no implicit Mask/Image
  conversion. Parameters: `layer` (the Cryptomatte type; empty, the default since #487, reads the first
  Cryptomatte layer the channel set carries) and `mattes` (the
  selection, in the common matte-list syntax: comma-separated, `\,` escapes a comma, `<1.23e-05>` or
  `<0xHEX>` is a raw ID). A muted Cryptomatte isolates nothing: its Mask is empty and its Image
  passes through.
- *As a matte.* The `mask` output (like any Mask output) shapes a layer as a matte layer mask,
  mattes a layer as a node track matte, limits an effect through its mask input and combines with
  other Masks in Mask Combine (#383; image-filters.md, "Matte Workflow"). Editing the selection
  re-renders the layers and effects it shapes.
- *IDs.* A name's ID is MurmurHash3_x86_32 (seed 0) of its UTF-8 bytes used as Float32 bits; an ID
  whose exponent bits are all zero or all one has bit 23 flipped. The selection resolves each entry
  as a raw ID, else through the part's `cryptomatte/<key>/manifest` (the key whose `name` is the
  layer), else by hashing the name, so names work without a manifest. A manifest that is not a
  JSON object of names to hexadecimal IDs is reported as a warning and ignored (an entry whose ID
  is not hexadecimal is skipped), and a `manif_file` sidecar is reported; names then resolve by
  hash.
- *Ranks.* The layer's rank pairs are `<layer>NN` channels in rank order: `00` R/G, `00` B/A, `01`
  R/G, ... (`R/G/B/A`, `r/g/b/a` and `red/green/blue/alpha` suffixes). A missing layer is the
  warning "The channel set has no Cryptomatte layer named X" (or, for the empty default with no
  Cryptomatte layer at all, "The channel set has no Cryptomatte layer") and an empty matte.
- *Coverage.* For each pixel, in rank order, `c = clamp(c + (bits(id) is selected ? coverage : 0),
  0, 1)` in Float32, published as `(c, c, c, c)`. The CPU reference and the GPU kernel apply exactly
  this order, so they are bit-identical.
- *Host gather.* Host preparation reads only the rank channels of the layer (decode layout) into
  its rank layers as the file stores them: one data image per two ranks, on the union window of the
  rank channels in the anchor source's frame, holding the (ID, coverage) channels of ranks 2k and
  2k + 1 (the last layer of an odd rank count holds one pair) with the IDs' exact bits and zero
  outside a channel's window. A non-finite ID or coverage is repaired to 0 and counted. The layers
  do not depend on the selection, so every Cryptomatte of the same type in one scene build reads
  one gather (#549).
- *One read of the part (#549).* A scene build registers, before any leaf decodes, the channels
  its channel reads (Cryptomatte ranks, Shuffle and depth channels) take from each source part
  (content digest and part), and the Image leaves that decode the same part's colour layer. The
  first decode of the part reads all of them in one pass over its chunks
  ([media I/O](media-io.md), "Channels beside the colour layer"): the Image leaf's colour decode when
  it is prepared first, otherwise the first channel read, which then decodes the leaf's colour
  layer too and shares it with the leaf. Every channel read of the build takes that one set, and the
  operation cache keeps it as a decoded-media entry keyed by the source sample and the channel set,
  so a later build of the frame decodes nothing for it; the sequence read-ahead decodes it with each
  later member's samples. The media statistics count `channelSetsReadWithColour`,
  `channelSetDecodes` (a channel read decoded on its own) and `channelSetsReused`.
- *GPU.* Each rank layer is its own frame-sized RGBA32F data upload (R/G the ID bits and coverage
  of rank 2k, B/A those of rank 2k + 1: two ranks in 16 bytes per pixel, the file's own layout),
  so no Cryptomatte upload is larger than one RGBA32F frame of its size: the stacked rank image
  that pushed 2160p off the GPU route is gone, and the request's live bytes (usable device
  capacity minus headroom on the production routes) bound the layers and passes together.
  `CryptomatteCoverageV1`
  (`src/render/vulkan/shaders/cryptomatte_coverage.comp`) runs once per rank layer, in rank order,
  each pass reading its layer, a bounded buffer of the selected IDs and the previous pass's output
  (the matte so far, or the preview's unclamped colour and alpha, stored as binary32), so the last
  pass is bit-identical to the CPU reference; the proxy PointResampleV1 gather follows. The
  coverage contract requires `operation.CompiledCryptomatte` and `node.bloom.cryptomatte` with
  native parity; `bloom.runtime.gpu_cryptomatte_native` proves the native matte bit for bit
  against the CPU reference (manifest, no manifest, raw IDs, proxy, empty selection, one to four
  ranks), the Image output, selection-edit reuse, two mattes sharing one upload per layer,
  cancellation and the budget.
- *Cache identity.* The CPU result key is `cryptomatte-v1` with the sources' cache keys (their
  content digests cover the manifest), the layer and the matte list. Each GPU rank layer upload is
  keyed by the source decode identities (the source sample), the rank channels and the layer's
  index only, so a selection edit, a second matte of the same type and a later frame that reads
  the same source sample reuse the resident layers; each matte resolves its IDs through the
  gather's manifest, and each coverage pass's key carries the IDs and the previous pass's key.
- *Viewer.* A Viewer inspection with no channels shows a pixel output port directly; the channel
  menu offers "Cryptomatte matte" for a selected Cryptomatte.
- *ID-color preview.* Selecting a Cryptomatte with Preview on (Properties, a session display
  choice, on by default) shows a viewer-only plan of the node's `preview` inspection port: a
  `CompiledCryptomatte` with `preview` set, whose pixel is, per rank in rank order, `color += tint x
  coverage` with `tint_k = 0.15 + byte_k x s` of the ID bits (bits 16-23, 8-15, 0-7; `s` the
  binary32 constant `0x3B5A740E`, 0.85/255, so no division can differ) brightened to `0.5 tint +
  0.5` for a selected ID, `alpha = clamp(alpha + coverage, 0, 1)` and the color clamped to alpha.
  With an image input it is an Image Mix (factor 0.5) whose base is the preview and whose second
  input is the image, so a data-only or absent image still shows the colors. The same
  CryptomatteCoverageV1 kernel runs it (mode 1); CPU and GPU agree bit for bit and the coverage
  contract carries a preview fixture. It never enters a render or export.
- *Eyedropper.* The Viewer point is mapped into the channel set's frame through the evaluated
  content-to-composition transform of the layer that displays the node (moves, scales, rotations,
  parents and the proxy included; the same mapping mask editing inverts), or taken as is in the
  node's preview or matte inspection. The worker reads the source frame at the node's own layer
  clock, resolved exactly as the evaluator resolves it (`detail::resolveClockTimes`), so a retimed
  or offset layer reads the frame it shows. It stores the dominant object's manifest name, or its
  raw ID without one; a point outside the source reports "No object under the cursor".
- *Data-only sources.* An Image source whose selected part and layer have no color channels has no
  Image output (ADR 0026 Q1): the evaluator records the information "No color layer — use
  Channels" and produces nothing, in preview and final render alike. GPU preparation matches it:
  the source publishes no command (counted in `GpuSceneMediaStatistics::imageNoColourLayerSources`)
  and the rest of the frame stays on the device, so a data-only selection never sends the frame to
  the CPU reference. Cryptomatte rank layers are never classified as color layers.

## Image Preview

Issue #490 adds `bloom.image-preview` (node schema 1, category Output), an inspection sink for
checking the image at any point in a graph:

- One optional input, `image`. Its declaration sets `InputPortDefinition::acceptsPixelKinds`, so
  besides an Image it also takes a Mask or a ChannelSet (`isAcceptedInputConnection()`; every
  connect and validation path asks it, through `CanonicalGraph::acceptsConnection()` where it has
  the destination). No other input sets the flag. It declares no output, so nothing connects from
  it.
- One inline selector, `display` (`bloom.image-preview.display`, Integer, constant only, not
  animatable): 0 RGBA, 1 RGB, 2 Alpha, 3 Red, 4 Green, 5 Blue, 6 Normalized. It chooses how the
  interface draws the preview and never reaches evaluation.
- Lowering `NodeLoweringKind::ImagePreview` lowers to nothing. The compiler reaches nodes only from
  the Composition Output (and value readouts), and a node with no output is never reached, so the
  composition's plan, its identity, RAM Preview frames, exports and headless renders are exactly
  those of the same graph without the node. `RenderReachability` never reaches it either, so an edit
  to it confines to `Unaffected` and invalidates no retained frame.
- The interface previews the input through a Viewer-only inspection plan
  (`runtime::ChannelInspection`): with no channels it shows the named node's Image or Mask output
  as the plan's output. A Cryptomatte's Mask inspection still reads only its channel set. For any
  other node the inspected node keeps every input, so its output is evaluated exactly as the
  composition evaluates it. A ChannelSet is shown by the ADR 0026 raw inspection of its default
  color layer (`defaultShuffleSlotChannel()`), or its alpha channel for the Alpha view. The source
  is the output the input connects to, followed through reroutes (`imagePreviewSource()`).
- `ui::ImagePreviewController` requests that plan through `CompositionPreviewController` at
  Background priority, as a proxy extent of the composition format sized to the drawn thumbnail
  or background (whole divisors, never above full resolution). The Viewer's display route is used
  for every view but Normalized: the GPU preview service when one is installed, with its own
  labelled CPU fallback. The RGB, Alpha and single-channel views are the Viewer's channel
  presentation of the packed display pixels. Normalized needs scene-linear values, so it takes the
  Viewer's analysis route (the CPU reference frame the pixel probe reads) and stretches the finite
  RGB minimum..maximum of the visible image to black..white. The interface labels any preview
  whose pixels did not come from the GPU route "CPU". Requests run on workers, one in flight per
  node. A change of input, time, size or display cancels the one in flight, and the newest identity
  is submitted once it ends. Images are kept (48 at most) by the inspected output, revision,
  time, extent, display and view, and are never inserted into the RAM Preview cache.
- GPU coverage: the lowering is classified `scope.inspection`, not a pixel operation. The node
  produces no pixels of its own, and what it shows is its upstream operations, each covered by its
  own Required entry, prepared by the Viewer's production GPU preview route. It therefore has no
  `node.bloom.image-preview` fixture.
