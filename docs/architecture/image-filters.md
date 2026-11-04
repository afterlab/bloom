# Image Filters

Status: implemented; grouped native and cross-platform qualification pending

Updated: 2026-11-04

## Gaussian Blur and Box Blur

`bloom.gaussian-blur` and `bloom.box-blur`, schema 1, are image-input/image-output
neighborhood effects. Both expose an animatable/drivable `radius` in full-resolution image pixels
and an `edge` selector: `0` Transparent (default), `1` Clamp. Radius zero is an exact storage alias.
They use the ordinary Add Node, parameter commands, undo/redo and project parameter codec. The
Properties radius field displays pixels; the Add menu lists both under Filters. Matte Blur (below)
reuses the same kernel for Mask coverage.

The v1 algorithm supports authored radius `0..256`, at most 513 coefficients per full-resolution
axis. Horizontal and vertical support independently use `ceil(radius * resolutionScale)`.
Upsampled support is bounded to 4096 samples per side, or 8193 coefficients/32,772 coefficient
bytes per axis. Larger sampled/upsampled requests fail explicitly; final rendering does not silently
reduce the radius. These finite loop/storage bounds apply to both CPU and GPU paths.

Gaussian uses sigma `radius/3`, evaluates the symmetric finite discrete kernel over that support,
and normalizes it. Box covers `[-radius, radius]` continuously over the same integer support: every
tap weighs 1 except, for a fractional radius, the two outermost taps, which weigh the radius's
fractional part, and the weights are normalized by their sum `2 * radius + 1`. Radius `0.01` is
therefore nearly the identity rather than a full 3-tap box, and an animated radius widens Box
smoothly instead of jumping at each integer (#460). The host prepares binary32 weights;
CPU and GPU use those identical coefficients with ordered binary32 fused multiply-add accumulation.
The horizontal intermediate is rounded to RGBA32F before the vertical pass. Both filter associated
RGB and alpha together, preserve signed/HDR RGB without a display clamp, and bound coverage to
`[0,1]` after each pass to remove normalization roundoff. A nonfinite output is a diagnosed failure.
No straight-RGB reconstruction or alpha-preserving color-transform behavior is implied.

Transparent samples outside the input data window as zero. Its output data window grows by the
horizontal and vertical support on both sides, retaining negative origins. Clamp replicates the
nearest input data-window sample and preserves the input data window. Both preserve display window,
pixel aspect and process encoding. Display-window edges never replace data-window edges. Content
bounds include the transparent halo. ROI evaluation conservatively retains the complete upstream
support and clips at the requested output; ROI pixels must equal the same region of a full render.

CPU evaluation allocates a horizontal intermediate and vertical output only after their combined
storage fits the request's remaining pixel budget. It checks cancellation at each row, at bounded
pixel intervals, and between passes, reporting progress across both passes. Invalid sampled radii,
overflow, allocation/budget refusal, cancellation and nonfinite pixels publish no partial image.
Radius, algorithm, edge, sampled coefficients, source identity, resolution and ROI participate in
normal operation/scene identity; animated and driven radius propagates time dependence.

GPU production preparation emits a resident transparent-halo padding command where needed, then
two `ProcessNeighborhood` compiled shader commands in the existing scene program lifecycle.
The host generates coefficients and shader text only; both pixel passes read and write resident
RGBA32F. Preview and final routes use the same scene builder/executor. Existing native program
admission, cache budgets, cancellation, fence retirement and safe shutdown govern those commands.
No blur pixels are CPU-uploaded as a purported GPU implementation. Missing tools or device support
returns explicit Unsupported and the normal diagnosed CPU fallback on Linux, macOS and Windows.
Native numeric parity and platform qualification remain unverified until the grouped supervisor gate.

The direct CPU test compares the separable output against an independent two-dimensional
convolution on negative-origin, signed/HDR, partial-alpha input, and checks proxy support, storage
refusal and cancellation between passes. Evaluator tests cover radius-zero identity, warm reuse,
radius/edge invalidation, animation and ROI/full-render equality. Required coverage fixtures include
both authoring nodes and the shared kernel. The native image-effect harness contains Gaussian/Box,
Transparent/Clamp, odd nonuniform proxy/ROI, exact-alpha/RGB oracle, warm zero-dispatch and changed-
radius cases. Native execution is deferred to the grouped gate; fixture presence is not parity proof.

## Matte Blur

`bloom.matte-blur`, schema 1 (#337), filters an ADR 0028 Mask: its required `input` and its
`mask` output are `SocketValueKind::Mask`, so it connects from the Mask node or Cryptomatte's
matte and feeds Image Mix's `mask`, never an Image socket. It reuses the Gaussian Blur controls and
schema keys unchanged: animatable/drivable `bloom.blur.radius` (`0..256` full-resolution pixels,
default `0`) and the constant `bloom.blur.edge` selector (`0` Transparent, default; `1` Clamp). It
is Gaussian only; there is no kernel selector. The Add menu lists it under Filters.

It lowers to the same `SeparableBlurKernel` with `BlurChannels::Coverage`. The coefficients,
supports, bounds, proxy scaling, halo/data-window, ROI, budget, cancellation and failure rules are
exactly the Gaussian Blur rules above. Coverage reads only the input alpha, accumulates it with the
same ordered binary32 fused multiply-add as the Rgba path, clamps it to `[0,1]` after each pass, and
writes it to every channel. The output is therefore always the Mask transport (premultiplied white)
even if an input's RGB differs from its alpha, and its coverage is bit-identical to the alpha a
Gaussian Blur of the same input and radius produces. Transparent treats coverage outside the input
data window as zero and grows the window by the halo; Clamp extends the edge coverage. Radius zero
is an exact storage alias.

Muting Matte Blur passes its Mask through (the mute rule's carried input is the first Mask input of
a node with only Mask outputs, in both the compiler and render reachability). An empty upstream Mask
makes Matte Blur empty, and an empty Mask connected to Image Mix is materialized as the transparent
image: zero coverage.

The channel policy enters the effect, operation and scene identity only when Coverage, so every
Gaussian/Box key keeps its earlier bytes. The GPU route emits the same two resident
`ProcessNeighborhood` commands from a Coverage variant of the generated program (semantics
`bloom.render.separable-blur.coverage.v1`, its own content identity and prepare key), through the
same scene builder, executor, admission, caches, cancellation and retirement for preview and final
render. It is classified by the Required `node.bloom.matte-blur` coverage fixture (Mask node ->
Matte Blur -> Image Mix mask, Transparent and Clamp, full frame and ROI) under the existing
`effect.SeparableBlurKernel`; there is no exception.

Tests: the direct CPU test compares Coverage with an independent two-dimensional convolution of
alpha over signed/HDR RGB, checks the all-channel write, windows, budget refusal, cancellation
between passes and the generated program. Evaluator tests cover identity, warm reuse, a memo key
separate from the Rgba blur's (with bit-identical alpha), ROI, animation and radius/edge
invalidation. Compiler tests cover lowering, mute pass-through and an empty upstream Mask; command,
Properties and archive tests cover add, wiring, undo/redo, refusals and reopen. The native
image-effect harness runs Coverage through the Gaussian Transparent/Clamp, proxy/ROI, warm
zero-dispatch and changed-radius cases at the 2e-6/exact-alpha gate.

## Matte Erode and Matte Dilate

`bloom.matte-erode` and `bloom.matte-dilate`, schema 1 (#338), shrink and grow an ADR 0028 Mask.
They have exactly Matte Blur's sockets and controls: a required Mask `input`, a Mask `mask` output,
the animatable/drivable `bloom.blur.radius` (`0..256` full-resolution pixels, default `0`) and the
constant `bloom.blur.edge` selector (`0` Transparent, default; `1` Clamp). The Add menu lists both
under Filters. Radius zero is an exact storage alias, and the supports, proxy scaling, bounds,
budget, cancellation and failure rules are the Gaussian Blur rules above.

They lower to the same `SeparableBlurKernel` with `BlurChannels::Coverage` and the morphology kinds
`SeparableBlurKind::Erode` and `Dilate` (refused with any other channel policy). The structuring
element is the separable square of `2 * ceil(radius * scale) + 1` taps per axis: whole taps weigh
exactly 1, and for a fractional radius the two outermost taps weigh the fraction, so an animated
radius grows smoothly. Each pass reads only alpha. Dilate starts at 0 and takes
`max(m, w * a)`; Erode starts at 1 and takes `min(m, fma(w, a, 1 - w))`, a weighted tap blending
its coverage toward the neutral 1. Both clamp to `[0,1]` and write the coverage to every channel,
so the output is always the Mask transport. An integer radius is therefore an exact square
minimum/maximum of coverage.

Transparent reads coverage outside the input data window as zero: Dilate grows the window by the
halo, while Erode keeps the input window (its halo would hold only zeros) and erodes from the data
window's edges. Clamp extends the edge coverage and keeps the input window for both. A muted
filter passes its Mask through, and an empty upstream Mask stays empty, as for Matte Blur.

The kind enters the operation and scene identity through the kernel kind and its unnormalized
weights, and the prepare key through an appended kind (absent for Gaussian/Box, whose keys keep
their bytes). The GPU route emits the same two resident `ProcessNeighborhood` commands from a
morphology variant of the generated program (semantics
`bloom.render.separable-erode.coverage.v1` / `bloom.render.separable-dilate.coverage.v1`), through
the same scene builder, executor, admission, caches, cancellation and retirement for preview and
final render. Both nodes are classified by the Required `node.bloom.matte-erode` and
`node.bloom.matte-dilate` coverage fixtures (Mask node -> filter -> Image Mix mask, Transparent and
Clamp, full frame and ROI) under the existing `effect.SeparableBlurKernel`; there is no exception.

Tests: the direct CPU test compares both against an independent two-dimensional square
minimum/maximum (with the fractional outer taps) over signed/HDR RGB, the all-channel write, the
windows at the image boundary, budget refusal, cancellation between passes and the generated
programs. Evaluator tests cover identity, warm reuse, radius/edge/kind invalidation, animation and
ROI. Compiler, command, Properties and archive tests cover lowering, mute pass-through, add, wiring,
undo/redo, refusals and reopen. The native image-effect harness runs both kinds through the
Transparent/Clamp, proxy/ROI, warm zero-dispatch and changed-radius cases at the 2e-6/exact-alpha
gate.

## Luma Key

`bloom.luma-key`, schema 1 (#338), keys an Image into an ADR 0028 Mask: a required Image `input`
and a Mask `mask` output, so it feeds Image Mix's `mask`, Matte Blur, Matte Erode or Matte Dilate,
never an Image socket. Its controls are the animatable/drivable `low` (`bloom.luma-key.low`,
default `0`) and `high` (`bloom.luma-key.high`, default `1`) luma thresholds, each finite in
`[-1e6, 1e6]`, and the constant `invert` Boolean (`bloom.luma-key.invert`, default false). The Add
menu lists it under Compositing.

The luma is the track-matte Luma: Rec. 709 weights `0.2126, 0.7152, 0.0722` over scene-linear
colour, unclamped, so HDR and negative values key by their real luma. The coverage is the pixel's
alpha times a linear ramp of its straight-colour luma from `low` (zero) to `high` (full), so partial
alpha scales the key and a transparent pixel has no coverage under either polarity; `invert`
reverses the ramp. `high <= low` is a hard key at `low` (`luma >= low` is covered, or `luma < low`
when inverted). Both routes evaluate it without a per-pixel division over premultiplied RGBA:
`luma_p = fma(0.0722, b, fma(0.7152, g, 0.2126 * r))`, `d = fma(-pivot, a, luma_p)` and coverage
`clamp(d * slope, 0, a)`, with `pivot = low, slope = 1/(high - low)` (or `pivot = high,
slope = -1/(high - low)` when inverted) prepared once in binary32 on the host. The coverage is
written to every channel over the input's data and display windows; outside the data window the
Mask is zero. A muted Luma Key is an empty Mask (an Image never passes through as a Mask), and an
empty upstream Image keys to an empty Mask.

The prepared mode, pivot and slope form the effect identity; the thresholds, invert and source
identity enter the operation key, and animated or driven thresholds propagate time dependence. ROI
evaluation is pointwise. The kernel is the `LumaKeyKernel` alternative, classified Required as
`effect.LumaKeyKernel` and covered with the Required `node.bloom.luma-key` fixture (an image keyed
by ramp, inverted ramp and hard key, read by Image Mix's mask, full frame and ROI). The GPU route
emits one resident `ProcessNeighborhood` command from one fixed program (semantics
`bloom.render.luma-key.v1`) whose storage buffer holds the prepared values, so keyed thresholds
change only data and the command identity, never the shader. It runs through the same scene
builder, executor, admission, caches, cancellation and retirement for preview and final render.

Tests: the direct CPU test checks the ramp, inverted and hard keys against an independent
double-precision luma over HDR, negative and partial-alpha pixels, the windows, budget refusal,
cancellation and the generated program. Evaluator tests cover warm reuse, threshold/invert
invalidation, animation and ROI. Compiler, command, Properties and archive tests cover lowering,
mute and empty propagation, add, wiring, undo/redo, refusals and reopen. The native image-effect
harness runs ramp, inverted, hard and HDR keys plus a warm zero-dispatch and a changed-threshold
run at the 2e-6/exact-alpha gate.

## Levels, Curves and Hue/Saturation

`bloom.levels` and `bloom.hue-saturation`, schema 1 (#340), and `bloom.curves`, schema 2 (#493),
are Image-in,
Image-out grades listed under Color in the Add menu. None of them performs a colour transform or
reads the working colour space: the values are graded exactly as stored, so a Raw data pass (Z
depth, mist, normals, position) interpreted with the Raw input colour space can be normalised or
remapped directly. They lower to the `LevelsKernel`, `CurvesKernel` and `HueSaturationKernel`
alternatives; `color_grade_image_effect.hpp` holds the one CPU reference and the GPU program text.

Levels and Curves address five channels in a fixed order: the RGB master, then red, green, blue
and alpha. Red, green and blue are graded by the master first and then by their own channel; alpha
is graded by the alpha channel only. Both operate on straight colour: each RGB value is divided by
the pixel's alpha (a zero-alpha pixel's straight colour is zero), graded, and re-associated by the
graded alpha. Graded alpha is always limited to `[0, 1]`, the storage invariant; a graded alpha of
zero publishes transparent black. An alpha transfer can therefore lift transparent pixels inside the
input's data window, so the content bounds of a grade are its data window. The windows and pixel
aspect are preserved and no display clamp is applied to colour.

Levels has five animatable/drivable Scalars per channel, each its own socket: `<channel>InputBlack`
(default 0), `InputWhite` (1), `Gamma` (1), `OutputBlack` (0) and `OutputWhite` (1), with schema
keys `bloom.levels.<channel>.<field>`. Black and white points are any finite value within
`[-1e6, 1e6]`; gamma is `[0.01, 100]`. A channel maps `x` to `t = (x - inputBlack) /
(inputWhite - inputBlack)` (an inverted range is allowed), raises a positive `t` to `1 / gamma`
(zero and negative `t` stay linear, so negative values never produce NaN), and returns
`outputBlack + t * (outputWhite - outputBlack)`. Values beyond the input range extrapolate, so HDR
and out-of-range data keep their slope. An input range too narrow for binary32 (including an empty
one) is a hard step at input black. The constant `clamp` switch (`bloom.levels.clamp`, default
off, no socket) limits each graded RGB value to `[0, 1]`.

Curves (schema 2, `grade_curves.hpp` and `bloom/runtime/grade_curve.hpp`) has nine animatable
Path parameters used as point lists, with no sockets, then the constant clamp. Each value is an
open Path of Corner anchors without handles; an anchor's point is one curve point `(input, output)`
and at most `kMaximumCurvePoints` (256) are allowed. In order (roles, schema keys
`bloom.curves.<key>`):

| Curve | Role / key | Input | Output | Default |
| --- | --- | --- | --- | --- |
| RGB master, red, green, blue, alpha | `master`.. `alpha` | value, `[-1e6, 1e6]` | value, `[-1e6, 1e6]` | `(0,0), (1,1)` |
| Hue vs Hue | `hueVsHue` / `hue-vs-hue` | hue, `[0, 1]` | hue shift in turns, `[-0.5, 0.5]` | empty |
| Hue vs Saturation | `hueVsSaturation` / `hue-vs-saturation` | hue, `[0, 1]` | gain, `[0, 1000]` | empty |
| Hue vs Luma | `hueVsLuma` / `hue-vs-luma` | hue, `[0, 1]` | gain, `[0, 1000]` | empty |
| Luma vs Saturation | `lumaVsSaturation` / `luma-vs-saturation` | luma, `[-1e6, 1e6]` | gain, `[0, 1000]` | empty |

The points are narrowed to binary32 and sorted by input; a point whose input repeats an earlier
one is dropped, and a single distinct input is a constant. Every curve is the Fritsch-Carlson
monotone piecewise cubic (PCHIP, the tangents SciPy's `PchipInterpolator` computes) through its
points: it passes through every point, never overshoots between monotone points and is flat at a
local extremum. The ends differ by kind. A channel curve extrapolates linearly along its end
tangents, so values above 1 continue the curve for HDR. Luma vs Saturation holds its end values.
The three hue curves are periodic over the hue strip: a hue input of 1 is hue 0, interior and end
tangents take their neighbours across the wrap, and one segment joins the last point to the first
through 1. A channel curve that is empty or has two or more points all on `y = x` is an exact
pass-through, so the default and the schema 1 five-point identity are both identities; a hue or
Luma vs Saturation curve that is empty or whose every output is neutral (0 for Hue vs Hue, 1 for a
gain) is the identity. Points too close together for binary32 segment coefficients refuse with a
diagnostic. The constant `clamp` switch (`bloom.curves.clamp`) limits each graded RGB value to
`[0, 1]`.

Each curve is animatable as a whole value (ADR 0028's whole-value Path keys): a keyed curve
interpolates point by point between keys with the same number of points and holds the earlier key
otherwise. A curve is never driven; the graph is its only editor.

The hue curves read one colour model, chosen to agree with Hue/Saturation: the Filter Effects
luminance weights `0.213, 0.715, 0.072`, and a chroma plane `w . c = 0` spanned by `e1`, the chroma
of pure red, and `e2`, the first column of the Filter Effects `hueRotate` sine matrix projected onto
the plane (the direction Hue/Saturation turns red by a quarter turn). A colour's chroma
`c = RGB - L` has least-squares coordinates `(a, b)` in that basis, folded into two rows applied to
RGB. Its hue is the diamond angle of `(a, b)` in turns (0 at red, a quarter at `+e2`), which needs
only ratios, so CPU and GPU hues agree exactly; greys (`R == G == B`) have no hue and pass the hue
block unchanged. On the straight colour after the channel curves, in order:

1. Hue vs Hue moves `(a, b)` to the diamond direction of `hue + shift` at the same Euclidean length
   and adds the change along `e1, e2`, so luminance is unchanged.
2. Hue vs Saturation and Luma vs Saturation multiply into one gain `g` (Luma vs Saturation reads the
   luma before the hue shift), applied as the Filter Effects `saturate` form `L' + g (RGB - L')`.
3. Hue vs Luma scales RGB by `1 + (v - 1) t` with `t = s / (s + |L|)` and `s = |a| + |b|`, so the
   gain fades out toward neutral colours and never divides by zero.

Then the clamp, the alpha curve and premultiplication by the graded alpha. No colour conversion
happens inside the node; HDR and negative values pass through every step, and the hue curves never
change alpha.

Hue/Saturation has an animatable/drivable `hue` (`bloom.hue-saturation.hue`, degrees, default 0,
within `[-1e6, 1e6]`) and `saturation` (`bloom.hue-saturation.saturation`, a factor, default 1,
within `[0, 1000]`). Its colour model is the CSS/SVG Filter Effects Module Level 1 `feColorMatrix`
pair: the `hueRotate` matrix for the hue, then the `saturate` matrix, multiplied in double and
narrowed once. Both use that standard's luminance weights `0.213, 0.715, 0.072`: any hue keeps
neutrals exactly and that luminance to within the standard's rounded coefficients (about 0.1%), and
saturation zero is the luminance on every channel.
The matrix is linear, so it is applied directly to associated RGB: alpha is unchanged, HDR and
negative values scale linearly and are never clamped, and no unpremultiply is needed.

Each grade prepares one packed binary32 table on the host (`PreparedColorGrade`): per Levels
channel the pivot, reciprocal range, inverse gamma, output black and range; per Curves channel the
control vec4 `[clamp, hueActive]`, the luma weights, the two opponent rows and `e1`, `e2`, then a
header `[n, identity, base, ends]` per curve and, at `base`, its knots, `n + 1` segment origins and
`n + 1` cubic polynomials `a + dx(b + dx(c + dx d))` (the extrapolation, hold or wrap segments
first and last), each list padded to whole vec4s; for Hue/Saturation the three matrix rows. A curve
finds its segment by binary search over the knots. A schema 1 five-point curve prepares the same
segments as before, so existing Curves output is unchanged. The CPU reference applies
that table with the same binary32 operations, ordered `fma`, and correctly rounded division. A grade
whose every channel is the identity and that does not clamp (the defaults, zero hue with unit
saturation) aliases its input exactly. A non-finite straight value or result is a diagnosed
failure, never a published pixel; a value outside its domain fails the frame explicitly.

The prepared table forms the effect cache identity; the clamp switch and every operand's resolved
value enter the operation key, and animated or driven operands propagate time dependence. ROI
evaluation is pointwise. The CPU reference checks cancellation at every row and every 4096 pixels,
reports per-row progress, and allocates its output only within the request's pixel budget;
cancellation, budget refusal and failures publish no partial image.

The GPU route emits one resident `ProcessNeighborhood` command from one fixed program per kind
(semantics `bloom.render.levels.v1`, `bloom.render.curves.v1`, `bloom.render.hue-saturation.v1`)
whose storage buffer holds the prepared table, so animated values change only data and the command
identity, never the shader. The straight-colour grades use the wrapper's correctly rounded division,
matching the CPU. The command runs through the same scene builder, executor, admission, caches,
cancellation and retirement for preview, still and sequence export, video export and headless
render. The kernels are classified Required as `effect.LevelsKernel`, `effect.CurvesKernel` and
`effect.HueSaturationKernel`, and covered by the Required `node.bloom.levels`, `node.bloom.curves`
and `node.bloom.hue-saturation` fixtures (an HDR, signed, partial-alpha gradient, full frame and
ROI) and the Required feature fixtures `feature.curves.points` (nine points on a channel),
`feature.curves.hue_vs_hue`, `feature.curves.hue_vs_saturation`, `feature.curves.hue_vs_luma` and
`feature.curves.luma_vs_saturation`; there is no exception. Gamma uses the device `pow`, and Hue vs
Hue the device `sqrt`, so a gamma other than 1 on the alpha channel and a hue shift match the CPU
within the 2e-6 gate rather than bit-exactly.

None of the three has a mask input yet: the shared mask input for image effects belongs to #383.

The curves are edited in a graph (`bloom/ui/curve_graph.hpp`), two of them in Properties (the
channel curves, tabs RGB/R/G/B/A, and the hue curves, tabs Hue/Hue, Hue/Sat, Hue/Luma, Luma/Sat),
and the node card draws the channel curves read-only. A click adds a point and the drag that
follows moves it; a press that moves nothing commits nothing; Delete or a double-click removes a
point (a channel curve keeps two); Escape, the right button, losing focus or hiding cancels. A drag
is a session value edit (ADR 0017): the graph and the Viewer follow it through the request-scoped
override and the release commits one command, or a key at the playhead on a keyed curve. The graph
draws the identity diagonal (a neutral line for the other curves) and the 0..1 box; a channel
curve's square view grows to hold every point plus 25% headroom above 1, a gain view spans 0..2
and Hue vs Hue half a turn either way, and the hue curves draw the hue strip behind them.

Behind a channel curve and Luma vs Saturation the graph draws the node input's luminance
histogram, on by default as session state. It is requested from `ImagePreviewController` only by
the graph's paint, so a hidden graph asks for nothing: the controller evaluates the node's `input`
source (`inspectedInputSource`) through `CompositionPreviewController::submitNodePreview` at 256
pixels on the longest side, on the GPU display route where it is available (labelled CPU
otherwise), and a worker bins the displayed luma into 128 bins scaled to the fullest. It is
display-referred, a guide to where the image's values sit rather than a scene-linear measurement,
and never touches the render output.

Tests: the direct CPU test compares Levels and Curves against independent double-precision
formulas (Curves in the Hermite basis, with any number of points, periodic hue curves and the hue
colour model computed independently) over HDR, negative and partial-alpha pixels with clamp on and
off, normalises a Raw depth pass, and checks steps, identities, point order, constants, domains,
windows, budget, cancellation and the generated programs; Hue/Saturation is compared against the
Filter Effects matrices with neutral and luminance preservation. Evaluator tests cover the
reference over a composited baseline, warm reuse, ROI, value and clamp invalidation, animation,
identity aliasing and refusals. Compiler, registry, command, Properties and archive tests cover
lowering, mute pass-through, the definitions, add, wiring, undo/redo, refusals and reopen. Scene
preparation checks one command per grade, that a changed value reuses the compiled shader, and
identity aliasing; the native image-effect harness runs every kind over signed/HDR/partial-alpha
solids at the 2e-6/exact-alpha gate with warm zero-dispatch and changed-value runs.

## Separate Channels and Combine Channels

`bloom.separate-channels` and `bloom.combine-channels`, schema 1 (#479), split an image into its
channels and build an image from channels. Both are listed under Color in the Add menu and work on
any Image: footage, a nested composition, a generator or a processed result, and render passes
such as mist, Z depth, normals, motion vectors and IDs. Every value is the stored Float32 channel:
nothing is clamped, rounded or colour converted, so a Raw data pass moves between channels exactly
as stored. Only alpha is limited to `[0, 1]`, the storage invariant. Neither node reads the working
colour space.

Separate Channels has one required Image input, `input`, and four Image outputs, `red`, `green`,
`blue` and `alpha`. Each output is a grey image, R = G = B = the stored channel and A = 1, over the
input's data and display windows with its pixel aspect. The channels are read as stored, that is
premultiplied: a half-transparent pixel's `red` output is its associated red.

Combine Channels has four optional Image inputs in the same order, `red`, `green`, `blue` and
`alpha`, and one Image output, `image`. Each input has a constant source dropdown (no socket):
`redSource`, `greenSource`, `blueSource` and `alphaSource` (schema
`bloom.combine-channels.source`; stored values Red 0, Green 1, Blue 2, Alpha 3, Luma 4), which
picks the stored channel of that input's image, or the Rec. 709 luma of its stored colour
(`0.2126 R + 0.7152 G + 0.0722 B`, computed in double and narrowed once). The defaults are Red,
Green, Blue and Red: every grey image carries its value in red, so Separate Channels wired
straight into Combine Channels reproduces the input bit-exactly. An unconnected red, green or blue
is 0 and an unconnected alpha is 1, so the alpha is optional. The constant `alphaMode` dropdown
(`bloom.combine-channels.alpha-mode`) declares the alpha association of the combined colour:
Premultiplied (0, the default) takes red, green and blue as already associated with the alpha, as
Bloom stores images and as Separate Channels outputs them; Straight (1) multiplies them by the
alpha. A combined alpha of zero publishes transparent black, because a premultiplied image cannot
hold colour at zero coverage.

The first connected input owns the output's data and display windows and pixel aspect; another
input is read at the same global pixel, and is 0 outside its own data window or when it has no
image at the requested time. When the first connected input has no image, or no input is
connected, Combine Channels produces no image. A muted node passes its first image input through.

The compiler lowers each reachable Separate Channels output to one `ChannelSelectKernel` (four
slot sources: a stored channel, Luma, Zero or One, and a premultiply flag). Combine Channels lowers
to a Channel Select of its first connected input, folding every other input that reads the same
image into that select, then one `ChannelCombineKernel` (slot, source channel, premultiply) per
remaining connected input in red, green, blue, alpha order; the Straight multiply is on the last
operation. `channel_image_effect.hpp` holds the one CPU reference and the GPU program text over
`render::storedImageChannel` and `render::setImageChannel`. ADR 0026's Shuffle, Channel Copy and
Remove are not reused: they rename and copy named channels of a multi-channel file before an image
exists, whereas these nodes work on the composited Rgba32f image of any upstream node.

The prepared mapping forms the effect cache identity, and both inputs' content identities enter a
combine's operation key. ROI evaluation is pointwise. The CPU reference checks cancellation at every
row and every 4096 pixels, reports per-row progress and allocates its output only within the
request's pixel budget; cancellation, budget refusal and failures publish no partial image. A red,
green, blue and alpha select without premultiply aliases its input.

On the GPU a Channel Select is one resident `ProcessNeighborhood` command from one fixed program
(semantics `bloom.render.channel-select.v1`) whose storage buffer holds the mapping, so a changed
dropdown changes data and the command identity, never the shader; its Luma uses the same widened
two-sum accumulation as ImageCombineV1. A Channel Combine is the `ImageCombineV1` Set Channel mode
(mode 11, slot in bits 4-5 of the channel word and premultiply in bit 6) over the two resident
images. Both run through the same scene builder, executor, admission, caches, cancellation and
retirement for preview, still and sequence export, video export and headless render. The kernels
are classified Required as `effect.ChannelSelectKernel` and `effect.ChannelCombineKernel`, and
covered by the Required `node.bloom.separate-channels` and `node.bloom.combine-channels` fixtures
(an HDR, signed, partial-alpha gradient, full frame and ROI); there is no exception.

To make an image search find the image nodes first, the Color value nodes are named as values:
Separate Color (Value) and Combine Color (Value) (`bloom.separate-rgba`, `bloom.combine-rgba`),
Separate HSV (Value), Combine HSV (Value), Hue Shift (Value) and Luminance (Value), all listed
under Color Values. Only display names and Add menu sections changed; type ids, ports and saved
projects are unchanged.

Tests: the direct CPU test checks the exact stored greys of HDR, signed, partial-alpha,
transparent and Raw data pixels, Luma, an exact Separate-to-Combine round trip, Straight
association, the alpha clamp, a smaller or absent second image, domains, budget, cancellation,
identities and the program. Evaluator tests cover reference parity, warm reuse, ROI, invalidation,
identity aliasing and malformed plans; compiler tests cover lowering, folding, mute and output
inspection; registry, command, Properties, search and archive tests cover the definitions, wiring,
dropdowns, refusals, undo/redo, ranking and reopen. The ImageCombineV1 native test compares Set
Channel against the CPU reference pixel for pixel, and the native image-effect harness runs Channel
Select over signed/HDR/partial-alpha solids at the 2e-6/exact-alpha gate with warm zero-dispatch
and changed-mapping runs.
