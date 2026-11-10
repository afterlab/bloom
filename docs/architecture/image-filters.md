# Image Filters

Status: implemented; grouped native and cross-platform qualification pending

Updated: 2026-11-05

## Gaussian Blur and Box Blur

`bloom.gaussian-blur` and `bloom.box-blur`, schema 1, are image-input/image-output
neighborhood effects. Both expose an animatable/drivable `radius` in full-resolution image pixels
and an `edge` selector: `0` Transparent (default), `1` Clamp. Radius zero is an exact storage alias.
They use the ordinary Add Node, parameter commands, undo/redo and project parameter codec. The
Properties radius field displays pixels; the Add menu lists both under Blur & Sharpen. Matte Blur
(below) reuses the same kernel for Mask coverage.

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

**Separate axes and per-channel radius (Gaussian Blur only).** Gaussian Blur has a constant boolean
`separate` (`bloom.blur.separate`, default off) and an animatable/drivable `radiusY`
(`bloom.blur.radius-y`, same domain as `radius`, default 0). With `separate` on, `radius` is the
horizontal radius and `radiusY` the vertical one; each axis has its own support and its own
coefficients, so the node blurs one axis alone when the other is 0 (the zero axis is a one-tap copy
and the GPU route dispatches only the other). The prepared coefficient sets were already one per
axis, so the CPU reference (`prepareSeparableBlur`'s optional vertical radius) and the GPU legs
(one per axis with more than one tap) read the same binary32 weights; the operation key carries the
vertical weights as before, so a changed `radiusY` is a new identity. A constant boolean
`perChannel` (`bloom.blur.per-channel`) and four animatable `redScale`, `greenScale`, `blueScale`,
`alphaScale` (`bloom.blur.<channel>-scale`, `[0, 1]`, default 1) give Channel Blur: each associated
channel is the input blurred by `radius * scale` (and `radiusY * scale`). The node lowers
(`appendChannelBlur`) to a blur at the widest scale, which owns the output window, then four
Channel Combine operations that put each channel of its own blur over it: five blurs and four
combines of existing primitives, so every operation keeps its CPU reference, GPU command, cache
identity, ROI, budget and cancellation. Alpha is blurred by its own scale like a colour channel.
A document saved without these parameters reads as a plain blur on both axes; no schema version
changed.

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
is Gaussian only; there is no kernel selector. The Add menu lists it under Keying & Mattes.

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
under Keying & Mattes. Radius zero is an exact storage alias, and the supports, proxy scaling, bounds,
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
menu lists it under Keying & Mattes.

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
Image-out grades listed under Color Correction in the Add menu. None of them performs a colour transform or
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
interpolates point by point between keys with the same number of points and morphs between keys whose
counts differ (path-value.md, "Morphing between different shapes"); a Hold key holds. A curve is
never driven; the graph is its only editor.

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

Hue/Saturation also has an animatable/drivable `vibrance` (`bloom.hue-saturation.vibrance`, `[-1, 1]`,
default 0) and a constant boolean `colorize` (`bloom.hue-saturation.colorize`, default off), both
optional additions that read as 0 and off in a document saved without them (no schema version
changed). **Vibrance** is applied after the matrix: with `chroma = clamp(max(r, g, b) - min(r, g, b),
0, 1)` of the input pixel and `Y = 0.213 r + 0.715 g + 0.072 b` of the result `c`, the result becomes
`Y + f (c - Y)` with `f = 1 + vibrance (1 - chroma)`, so a positive vibrance saturates dull pixels
(`f` up to `1 + vibrance`) and leaves fully saturated ones (chroma 1) and neutrals alone, and a
negative one mutes dull pixels the most. It uses no division, so the CPU and the GPU program run the
same binary32 operations. **Colorize** replaces the matrix by a rank-one one: every output is the
input's luma times one tint, `out_i = tint_i / L * (0.213 r + 0.715 g + 0.072 b)`, where the tint is
the hue `h` (degrees) at full chroma (`r = clamp(|h/60 - 3| - 1, 0, 1)` and its rotations for g and b)
blended toward white by `min(saturation, 1)`, and `L` is the tint's own luma so the picture keeps
its brightness. Hue and Saturation are the tint's hue and strength in this mode; alpha is untouched.
The program is `bloom.render.hue-saturation.v2` (the table gained a fourth vec4 for the vibrance).

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
(semantics `bloom.render.levels.v1`, `bloom.render.curves.v1`, `bloom.render.hue-saturation.v2`)
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

Levels is edited graphically (#494, #501) on its node card and in Properties by the same widget
(`bloom/ui/levels_graph.hpp`, a `NodeCardWidget`): a tab per channel (RGB, R, G, B, A; a dot marks
a channel that is not at its defaults) with the channel's keyframe diamond and a Values button, the
Histogram switch and a Reset button for the channel, the plot over the node input's luminance
histogram (the Curves histogram below, requested only while the plot paints and the switch is on),
with handles for the channel's input black, gamma and input white under the plot and for its output
black and white under an output bar, and the five values always shown under it ("Input": black,
gamma, white; "Output": black, white). The parameters are the numeric ones above, unchanged, so
existing output and identities are unchanged. The gamma handle sits at
`inputBlack + (inputWhite - inputBlack) * 0.5^gamma`, where the output is half way between output
black and white; dragging it maps back through `gamma = log(t) / log(0.5)`, limited to the gamma
domain. The plot spans `0..1` and grows to hold a black or white point outside it, plus 10%
headroom; a drag keeps the range it started with. The plot is one uniform grey with a grid line
every quarter and 0 and 1 marked on its foot. A handle drag follows the shared grading rules
(`bloom/ui/grading_controls.hpp`): it moves by the pointer's travel, so a handle never jumps; a
Shift-press on a handle's row jumps the nearest handle to the pointer; Ctrl drags at 0.1x and Shift
at 10x, a modifier change starting a new segment. Each drag is one session value edit and one
command on release; a press that moves nothing commits nothing; Escape, the right button, losing
focus or hiding cancels; a double-click resets a handle to its default, and on the plot the channel,
as one command. The right-click menu offers those resets, the channel's key (one command for its
five values) and Show Values. The card shows the widget and the Clamp row ("Clamp to 0-1") instead
of the 25 value rows; Alt-click the plot, or its Values button, to show the current channel's five
rows with their sockets, to link them. A linked value's row always shows.

The curves are edited in one graph (`bloom/ui/curve_graph.hpp`, `CurvesCardWidget`, a
`NodeCardWidget`) drawn the same on the node card and in Properties (#498): a tab bar picks the
channel curve (RGB/R/G/B/A) or the hue curves, whose curve a dropdown picks by its full name, with
the curve's keyframe diamond and a Preset menu beside it, the graph, and the selected point's input
and output always shown under it in their units (a hue in degrees, a gain as a multiplier). The card
has no rows for the curves and lists no points; Clamp is its one row. A click on the background adds
a point and the drag that follows moves it; a press on a point drags it relative to where it was
pressed, so it never jumps, Ctrl (Cmd) moves it at 0.1x, and a modifier change mid-drag starts a new
segment from where the point is. Dragging a point more than `Spacing::M` off the graph removes it
(dragging back restores it; a channel curve keeps two), as does Delete Point in the right-click menu
or the Delete key; the menu also offers Reset Curve and the curve's key at the playhead. A
double-click resets the curve; when its first click added a point, that commit is undone first, so
the reset is the double-click's one history step. Escape, the right button, losing focus or hiding
cancels a drag. A drag is a session value edit (ADR 0017): the graph and the Viewer follow it
through the request-scoped override and the release commits one command, or a key at the playhead on
a keyed curve; a preset or reset is one command the same way. The graph draws the identity diagonal
(a neutral line for the other curves) over one uniform grey with a grid line every quarter, the
range above 1 drawn like the rest (#501: no lighter 0..1 box); a channel curve's square view grows to hold
every point plus 25% headroom above 1, a gain view spans 0..2 and Hue vs Hue half a turn either way.
The hue curves draw the hue strip behind them, and every curve that is not a channel curve names its
axes and draws a band along each: the hue strip or a black-to-white luma ramp along the input, and
the turned hue (Hue vs Hue), grey to colour (a saturation gain) or black to white (a luma gain) up
the output.

The presets (`document::GradeCurvePreset`, `gradeCurvePreset()`) are point lists over 0..1: Linear
(0,0)(1,1); Medium Contrast (0,0)(0.25,0.18)(0.75,0.82)(1,1); Strong Contrast
(0,0)(0.25,0.12)(0.75,0.88)(1,1); Low Contrast (0,0)(0.25,0.31)(0.75,0.69)(1,1); Lighten
(0,0)(0.5,0.62)(1,1); Darken (0,0)(0.5,0.38)(1,1); Fade (0,0.1)(0.5,0.52)(1,0.92); Crush Blacks
(0,0)(0.12,0)(1,1); Invert (0,1)(1,0). A channel curve extends past its end points linearly, as
any curve does; a hue or Luma vs Saturation curve offers only Reset, its empty default.

A keyed curve is a whole-value Path curve: the compiler accepts it as a Path parameter's source
(it used to refuse it as "Animation curve type does not match") and the lowering compiles it inline
where the curve is sampled, so its keys select, move and delete in the timeline like any other.

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

## Grade

`bloom.grade`, schema 1 (#494), is an Image-in, Image-out primary grade listed under Color Correction, in the
manner of lift/gamma/gain primaries and the ASC CDL. It lowers to the `GradeKernel` alternative and
shares the grade machinery above (`color_grade_image_effect.hpp`): one host-prepared binary32
table, the CPU reference that applies it, and one fixed resident GPU program
(`bloom.render.grade.v1`) that reads it from its storage buffer.

Its 22 parameters are animatable, drivable Scalars, each its own socket, in this order (roles,
schema keys `bloom.grade.<wheel>.<component>` and `bloom.grade.<primary>`):

| Group | Roles | Default | Domain |
| --- | --- | --- | --- |
| Lift wheel | `liftMaster`, `liftRed`, `liftGreen`, `liftBlue` | 0 | `[-1000, 1000]` |
| Gamma wheel | `gammaMaster`, `gammaRed`, `gammaGreen`, `gammaBlue` | 0 | `[-8, 8]` |
| Gain wheel | `gainMaster`, `gainRed`, `gainGreen`, `gainBlue` | 1 | `[-1000, 1000]` |
| Offset wheel | `offsetMaster`, `offsetRed`, `offsetGreen`, `offsetBlue` | 0 | `[-1000, 1000]` |
| Temperature, Tint | `temperature`, `tint` | 0 | `[-1000, 1000]` (bar `-100..100`) |
| Contrast, Pivot | `contrast`, `pivot` | 1, 0.18 | `[0, 10]`, `[0.001, 1000]` |
| Saturation | `saturation` | 1 | `[0, 1000]` |
| Exposure | `exposure` (stops) | 0 | `[-32, 32]` |

The node works on scene-linear values exactly as stored: no colour conversion, no clamp, and alpha
unchanged. Each RGB value is divided by alpha (straight colour), graded, and multiplied by the same
alpha; a pixel with zero alpha passes unchanged. For channel `c` (red, green, blue), with
`L = liftMaster + liftC`, `G = gammaMaster + gammaC`, `K = gainMaster * gainC`,
`O = offsetMaster + offsetC`, in order:

1. Exposure and white balance: `x = s * 2^exposure * b_c`, with the gains in stops
   `b_r = 2^(T/200 + N/400)`, `b_g = 2^(-N/200)`, `b_b = 2^(-T/200 + N/400)` for Temperature `T`
   and Tint `N` (positive is warmer, positive tint is magenta; the product of the three gains is
   one, so a grey keeps its geometric mean).
2. Lift and gain: `y = K * (x + L * (1 - x))`, the common grading convention, which raises black to
   `K * L` and keeps white at `K`.
3. Offset: `y = y + O`.
4. Gamma: `y = y^(2^-G)` for `y > 0`; zero and negative values pass, so a positive gamma
   brightens the middle and a negative value never produces NaN.
5. Contrast about the pivot: `y = pivot * (y / pivot)^contrast` for `y > 0`, so the pivot stays
   put and black stays at zero; zero and negative values pass.
6. Saturation: `y = Y + saturation * (y - Y)` with the Filter Effects luma
   `Y = 0.213 r + 0.715 g + 0.072 b` (the weights Hue/Saturation and the Curves hue curves use);
   zero is the luma on every channel.

Steps 1 to 4 are the ASC CDL form per channel: slope `2^exposure * b_c * K * (1 - L)`, offset
`K * L + O` and power `2^-G`, which is how the host folds them. The prepared table is
`[slope r, g, b, 0] [offset r, g, b, 0] [power r, g, b, 0] [contrast, pivot^(1 - contrast),
saturation, 0] [luma weights, 0]`; the pixel is `t = fma(s, slope, offset)`, then
`pow(t, power)` and `scale * pow(t, contrast)` for a positive `t` when they are not one, then
`fma(t - Y, saturation, Y)` when saturation is not one, so a neutral step is skipped exactly. A
grade whose table is the identity (the defaults, or e.g. +1 stop with a 0.5 gain) aliases its
input. A value outside its domain, or a folded slope, offset, power or contrast scale outside
binary32, fails the frame with a diagnostic; a non-finite result is a diagnosed failure, never a
published pixel. The cache identity, operation key, time dependence, ROI, budget, cancellation and
progress follow the grades above. The GPU uses the device `pow`, so gamma and contrast match the
CPU within the 2e-6 gate rather than bit-exactly.

The kernel is classified Required as `effect.GradeKernel` and covered by the Required
`effect.GradeKernel` (the four wheels) and `node.bloom.grade` (the primaries, then every control
together) fixtures over the HDR, signed, partial-alpha gradient, full frame and ROI, through the
production builder; the native harness runs both over signed/HDR/partial-alpha solids at the
2e-6/exact-alpha gate with warm zero-dispatch and a changed-value rerun. There is no exception.

The node card and Properties draw the same widget (`bloom/ui/grade_wheels.hpp`, a
`NodeCardWidget`): the Lift, Gamma, Gain and Offset wheels two by two, then the primaries.
A wheel's puck shows the part of its red, green and blue that is not grey, in the chroma plane
perpendicular to grey with red at the vectorscope's 103 degrees (then yellow, green, cyan, blue
and magenta anticlockwise); the rim is a chroma length of 0.25 (Lift), 0.5 (Gamma, Gain) or 0.1
(Offset). The wheel is a donut (#501): the hue ring around an empty centre (half its radius) while
the colour is neutral, the centre filled with the faded hues once it is not, with only the puck and
its line from the centre on it. Only a press inside the disc starts a drag, and the puck moves by the
pointer's travel (1:1 in disc pixels, stopped at the rim), so it never jumps; a Shift-press jumps it
to the pointer, which it then follows; Ctrl (Cmd) moves it at 0.1x, and a modifier change mid-drag
starts a new segment from where the puck is. The arrow keys nudge it (a run of keys is one edit).
The drag writes the three values that put the puck there while keeping their mean, as one session
value edit over the three parameters and one command on release (a key at the playhead on an
animated value). A wheel's title row holds its name, its keyframe diamond (one command keys or
unkeys its four values), a Reset button (colour and level) and a Values button that shows its four
value rows with their sockets; Alt-click also toggles them. A double-click on the disc resets its
colour only. Under the disc the wheel's level is always readable: "Lift level" (and so on, its
tooltip saying what it does) with its signed value and unit (`+0.000` for Lift and Offset, `+0.00
st` for Gamma, stops of the power, `1.00 ×` for Gain), over a jog strip 88% of the disc wide: a
step a pixel (0.001 for Lift and Offset, 0.01 for Gamma and Gain), Ctrl 0.1x, Shift 10x, no end
stops short of the domain, the pointer wrapping at the screen edge as a scrub does; darker to the
left, a tick at neutral, a bar from neutral to the value and a chevron when the value lies past the
soft span. Under that, the red, green and blue values (two places shown, three stored), each
underlined in its channel's colour. The primaries are one row each, its name, its value with its
unit, its keyframe diamond and its socket beside the row: Temperature and Tint over blue-to-amber and
green-to-magenta tracks, Contrast (×) with a detent at 1, Pivot over a picture of the contrast curve
whose marker drags the pivot, Saturation (×) over grey to colour and Exposure (st) over whole-stop
ticks; their Reset button resets the six as one command. A card draws only inside its own
rectangle: the socket name of a row kept behind the widget is hidden with the row. On the card a
wheel's four rows, with their sockets, stay hidden until its Values button or Alt-click shows them;
a linked value's row always shows.

Hue/Saturation's card (`bloom/ui/hue_saturation_widget.hpp`) draws its Hue row over a hue strip:
the input spectrum above, and below it each input hue in the colour the node turns it into, through
the node's own matrix (`runtime::hueSaturationMatrix`, which the CPU reference prepares its kernel
from). Dragging the strip slides the output spectrum with the pointer and turns the hue, wrapping at
±180°. The Saturation row sits over a grey-to-colour slider with a detent at 1. Exposure, Offset and
Multiply (`bloom/ui/scalar_slider_widget.hpp`) draw their amount's row (Exposure in stops, Factor as
a multiplier) over a slider with a detent at neutral and ticks along its soft range (whole stops for
Exposure). These rows carry their sockets.

## Separate Channels and Combine Channels

`bloom.separate-channels` and `bloom.combine-channels`, schema 1 (#479), split an image into its
channels and build an image from channels. Both are listed under Channels in the Add menu,
beside the other channel nodes, and work on any Image: footage, a nested composition, a generator
or a processed result, and render passes such as mist, Z depth, normals, motion vectors and IDs. Every value is the stored Float32 channel:
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
under Color Values. Only display names and Add menu groups changed; type ids, ports and saved
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

## Drop Shadow, Glow and Light Wrap

`bloom.drop-shadow`, `bloom.glow` and `bloom.light-wrap`, schema 1 (#339), are compositing
recipes listed under Light & Shadow in the Add menu. Each has a required Image input, `input`, and an
Image output, `image`. None has its own pixel kernel: the compiler lowers each node to a chain of
the existing image primitives (`compositing_recipes.hpp`), and every value the chain derives from
the node's parameters (an offset, a knee bound, an octave weight, a gain) is a value-graph
operation the compiler appends after the value pass, folded to a constant when its operands are
constant. A keyframed or wired parameter therefore animates through the same value graph as any
other parameter. A derived value names no document parameter: like a compiler-synthesized constant,
it carries no parameter identity and claims none in plan validation, so each authored parameter
keeps the one kernel field it drives directly (Light Wrap's second use of its radius reads it
through the value graph, since an animation curve has one image-parameter owner). All three are premultiplication-correct and HDR-safe: colour is scaled straight
and re-associated by the primitive that owns it, nothing clamps colour, and the composites are the
Porter-Duff forms below. A muted node passes its input through.

Drop Shadow parameters and defaults: `color` (black, opaque; its alpha scales the opacity),
`opacity` 0.75 in `[0, 1]`, `direction` 135 degrees measured clockwise from up (down-right on the
y-down canvas), `distance` 10 pixels in `[0, 16384]`, `softness` 8 pixels in `[0, 256]`, and the
constant switch `shadowOnly` (off). The chain is a coverage Matte Blur of the input's alpha by
`softness` with a transparent edge, so the shadow grows past the input's data window; a Transform
2D translating by `(distance sin(direction), -distance cos(direction))`; Levels on that white
coverage with the straight colour's output white at `color` and the alpha's at
`opacity * color.alpha`; then a plain Merge of the input over the shadow. Shadow Only publishes the
shadow alone.

Glow parameters and defaults (Classic, the default mode; #506 adds the other modes and the shared
controls, see "Glow modes" below): `threshold` 1.0 (so only over-white HDR values glow by default),
`knee` 0.5 centred on the threshold, `intensity` 1 in `[0, 1000]`, `size` 64 pixels in `[0, 256]`,
`falloff` 0.6 in `[0, 1]`, `tint` (white; its alpha is ignored), the constant integer `octaves` 5 in
`[1, 8]` and the constant switch `glowOnly` (off). The optional `key` Image input (a render pass
or any image) replaces the input as the glow source, and the optional `mask` Mask input limits it.
Luma Key ramps the source's straight Rec. 709 luma from `threshold - knee/2` to
`threshold + knee/2` (a zero knee is a hard step), and Set Alpha with that coverage keeps the
source's straight colour at the ramped coverage: premultiplied colour times the ramp, never alpha
squared. A connected mask multiplies its coverage in (Multiply Alpha). Octave `j` of `n` is a
Gaussian blur with radius `size * 2^(j - (n - 1))`, so the widest is `size` and each further octave
halves it; the octaves are folded widest first by Image Mix so octave `j` carries the normalized
weight `falloff^j / sum(falloff^i)`: falloff 0 keeps only the tightest octave and 1 weighs
them all equally. Levels scales the glow's straight colour by `tint * intensity`, and a Merge entry
with the explicit Add mode adds it to the input (`co = cg + ci`, `ao = ag + ai - ag ai`). Glow Only
publishes the glow alone. An unconnected key or mask is omitted; one connected to an empty image is
a transparent image.

Light Wrap parameters and defaults: `radius` 16 pixels in `[0, 256]`, `amount` 0.75 in `[0, 1]`,
and the constant switch `wrapOnly` (off), with an optional `background` Image input. The
background is blurred by `radius` with a Clamp edge, so the frame edge does not darken it; the
foreground's alpha is blurred by `radius` with a transparent edge; Multiply Alpha by the inverted
spread coverage keeps the blurred background only where it spills over the foreground's edges;
Levels scales its alpha by `amount` (a separate gain after the hold-out, so a changed amount
re-runs only Levels); and a Merge entry with the explicit Atop
mode composites it onto the
foreground (`co = cw af + cf (1 - aw)`, `ao = af`), so the wrap only lightens or tints the
foreground within its own coverage. Wrap Only uses the In mode (`co = cw af`, `ao = aw af`) and
publishes the wrap alone. With no background there is nothing to wrap: the node passes the input
through, and Wrap Only produces a transparent image.

Every chain operation keeps its own cache identity, so a changed opacity, tint or amount re-runs
only Levels and the Merge, and a changed radius re-runs from the blur it drives
on. Windows follow the primitives: blurs grow transparent-edge data windows by their support, the
Merge's data window is the union of its entries and its display window the composition's. ROI,
cancellation, progress, budget and retirement are those of the primitives and the Merge; no recipe
publishes a partial image. On the GPU each primitive is its existing Required resident command and
the explicit Merge modes take the per-entry BlendV1 path, for preview, still and sequence export,
video export and headless render. The recipes are covered by the Required `node.bloom.drop-shadow`,
`node.bloom.glow` and `node.bloom.light-wrap` fixtures (an HDR, partial-alpha gradient over an HDR
background, full frame and ROI); there is no exception. Glow's coverage fixture cases key an HDR,
partial-alpha solid: Luma Key's coverage is a function of colour, and a generated gradient's colour
is within 2e-6 of the CPU reference on a device rather than bit-exact, so only a solid source meets
the exact-alpha gate. Decoded footage is bit-exact on both routes, so the Glow fixture also keys
a Physical glow from a translucent premultiplied OpenEXR in the working space (#545). Alpha keyed
from tolerance-qualified colour, such as a Glow of a generated gradient or of a device Gaussian
blur, is held to the same 2e-6 rather than exactly ([GPU backend](gpu-backend.md), reference-parity
profile); the native recipe test keys a Glow from the gradient under that rule.

Tests: the direct recipe test compares each chain and its variants (Shadow Only; a hard one-octave
Glow Only; a keyed, masked Glow; Wrap Only) with an independent double-precision reference (direct
2D Gaussians, the straight luma ramp, the octave sum and the Porter-Duff composites) at 1e-4, and
covers warm reuse, invalidation, ROI, animation, cancellation and budget refusal. Compiler tests
cover lowering, folding, octave radii and weights, the Merge modes, animated values and mute;
registry, model, command, Properties and archive tests cover the definitions, wiring, switches,
refusals, undo/redo and reopen. The scene-preparation test checks the Add, Atop and In blend
commands, and the native image-effect harness runs all three recipes against the CPU oracle at the
2e-6/exact-alpha gate with warm zero-dispatch and changed-value runs.

### Glow modes

`bloom.glow` has one node and three modes (#506), chosen by the constant `mode` dropdown: Classic
(0, the default and the glow described above), Physical (1) and Streak (2). The modes and the
shared controls below are optional parameters of the same schema-1 node: a Glow saved before them
binds none, lowers with their defaults and therefore renders exactly the Classic chain it always
did (the compiler test proves the plan identical). A new node binds them all. Opening a project
binds each one an older Glow lacks at its default (#534, see
[project format](project-format.md#glow-controls)), so every Glow shows the same Properties rows
and card sockets and still renders bit-identically. Every chain is still a recipe of
primitives with derived values in the value graph, so each value animates or is driven as usual.

**Physical** is a multi-scale, energy-conserving glow with an inverse-power falloff. Its point-spread
function (PSF) is modelled on physically based glare: Spencer, Shirley, Zimmerman and Greenberg,
"Physically-Based Glare Effects for Digital Images" (SIGGRAPH 1995), fit the eye's scatter with a
Gaussian core plus terms falling off as the inverse square and inverse cube of the angle. A PSF
`r^-p` is built as a Gaussian scale mixture: integrating Gaussians of width `s` against a weight
`s^(2-p)` per unit `log s` gives `integral s^(-p-1) exp(-r^2 / 2s^2) ds`, which is proportional
to `r^-p`. Discretized on the octave radii (each twice the last, as in the bloom pyramids of
Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare", SIGGRAPH 2014, and
Unreal Engine's bloom, which sums Gaussians of increasing size), octave `j` weighs `2^(j (2-p))`
relative to the tightest, so Physical reuses Classic's octave fold with the per-octave ratio
`2^(2 - exponent)`: `exponent` 2 (the default) weighs every octave equally, the inverse square; 3
halves each wider octave, the inverse cube. The weights are normalized, so the PSF integrates to one.
Physical keys the light energy: Channel Select makes the premultiplied colour opaque, Luma Key ramps
its Rec. 709 luma (the light a partly covered pixel emits) over the threshold and knee, a
connected mask multiplies the ramp, and Multiply Alpha takes `B = source * ramp`. Energy
conservation follows the normalized-kernel model of Unreal Engine's convolution bloom and the
"physically based bloom" write-ups (for example LearnOpenGL's), which redistribute light rather
than add it: Image Mix, under the ramp as its mask, moves the input's colour towards zero by
`min(intensity, 1)` with alpha untouched, and the glow carries `intensity * PSF(B)` (times the
tint). With a white tint and `intensity` up to 1 the frame's total light is unchanged (the energy
test checks it); above 1 the excess is added light. A connected `key` is a separate light source,
so nothing is removed from the input.

**Streak** makes `points` rays (constant, `[1, 16]`, default 4: a four-point star; 2 is an
anamorphic streak) at `angle + 360 k / points` degrees, measured clockwise from screen right
(default 0), each `length` pixels long (`[0, 1024]`, default 128). It keys like Classic. A ray is
Kawase's iterated streak filter ("Frame Buffer Postprocessing Effects in DOUBLE-S.T.E.A.L
(Wreckless)", GDC 2003): five chained Directional Blur passes of four taps, pass `p` spaced
`length * 4^p / 1023` with tap ratio `0.01^(4^p / 1023)`, so the product is one ray of 1024 taps
along `[0, length]` with the exponential weight `0.01^(t / length)` (the last tap weighs 1% of the
first), at a cost of twenty bilinear taps per pixel. Each ray carries `1 / points` of the light
(Levels on its alpha) and an Add Merge sums the rays, so the star's alpha is the union of the rays'
scaled coverage. `chromatic` (`[0, 1]`, default 0) disperses the rays: green reaches
`1 - chromatic / 3` and blue `1 - 2 chromatic / 3` of red's length, so a ray whitens at its source
and reddens towards its tip. Streak ignores `size`, `falloff`, `octaves`, `exponent` and the
gradient.

The new primitive, **Directional Blur** (`DirectionalBlurKernel`, Required in the GPU coverage
contract), is one pass over associated RGBA: tap `i` of 0..3 reads the input at
`c - i * spacing * (cos a, sin a)` (scaled to the evaluation resolution and snapped to 2^-20
pixel) with weight `decay^i / sum(decay^j)`. The host splits each tap into its four bilinear
texels, so a pass is sixteen weighted integer taps folded `sum = fma(texel, w, sum)` in order,
transparent outside the input, alpha limited to `[0, 1]`, on the CPU reference and in one fixed
resident program whose storage buffer holds the taps (a keyed angle or length never recompiles).
The data window grows to every pixel a nonzero tap reaches, only in the ray's direction; spacing 0
aliases its input. Spacing is bounded to `[0, 1024]` pixels and the decay to `(0, 1]`.

**Shared controls**, in every mode:

- `threshold` and `knee`, as in Classic.
- `radiusRed`, `radiusGreen`, `radiusBlue` (`[0, 1]`, default 1): each channel's glow uses that
  fraction of `size` (Streak: of `length`). When any differs from 1 (or `chromatic` is not 0 in
  Streak) the glow is built once per channel; Levels keeps each channel's colour while scaling its
  alpha to a third, Channel Select keeps that channel and an Add Merge sums the three, so every
  channel's colour is exact and the alpha is the union of the thirds.
- `tint`, and the colour gradient: with `gradient` on (default off), octave `j` of `n` is tinted
  with `tintGradient`'s colour at `j / (n - 1)` before the fold (the tightest octave takes the
  gradient's start, the widest its end), and the final Levels scales by `intensity` alone. Streak
  uses `tint`. `tintGradient` (node schema 2, #541) is an animatable gradient value of 1 to 64 stops
  (default white at 0 and (1, 0.5, 0.25) at 1), edited on the card and in Properties with the
  gradient editor; a constant gradient folds to constant octave tints, and a keyed one samples at
  the frame through a value-graph gradient sample per octave, as does one whose stops a value node
  drives (#543, project-format.md "Gradient Stop Links In Document 1.40"). Its two-stop default is
  exactly schema 1's `tint + (outerTint - tint) * j / (n - 1)`, which a schema 1 Glow opens as
  (project-format.md, "Gradient Colour Pairs In Document 1.39").
- Lens dirt: an optional `dirt` Image input adds the glow times the dirt's Rec. 709 luma coverage
  (Multiply Alpha, Luma), its colour scaled by `dirtAmount` (`[0, 1000]`, default 1), as Unreal
  Engine's bloom dirt mask does.
- `rollOff` (`[0, 100]`, default 0, off): a highlight roll-off of the whole glow layer (after the
  dirt), the new **Highlight Roll-off** grade (`HighlightRollOffKernel`, Required): associated
  colour `c` becomes `c / (1 + rollOff * max(L, 0))`, `L` its Rec. 709 luma, alpha unchanged. It is
  Reinhard's luminance operator (Reinhard et al., "Photographic Tone Reproduction for Digital
  Images", SIGGRAPH 2002) applied hue-preservingly: dim glow is nearly unchanged and bright glow
  approaches a luma of `1 / rollOff`. It is one fixed grade program whose amount is data, dividing
  with the wrapper's correctly rounded division so the device matches the CPU.

Defaults chosen for #506: mode Classic; exponent 2 (inverse square); points 4; angle 0; length
128 pixels; chromatic 0; per-channel radius 1; gradient off with a white to (1, 0.5, 0.25) tint
gradient;
dirt amount 1; roll-off 0.

Cache identity, cancellation, ROI and budget are the primitives': a changed intensity re-runs the
gains Levels and the Merge (Physical's removal only when `min(intensity, 1)` changes), a changed
roll-off only the roll-off, a changed length or angle the rays from their first pass. Directional
Blur needs its full input support under an ROI, like the other neighbourhood filters. The modes are
covered by the Required `feature.glow.physical`, `feature.glow.streak` and
`feature.glow.shared_controls` fixtures and the primitives by `effect.DirectionalBlurKernel` and
`effect.HighlightRollOffKernel`; there is no exception. Preview, still and sequence export, video
export and headless render all take the same resident commands.

Tests: the glow primitive test compares Directional Blur with a direct bilinear ray sum at several
angles, spacings and decays (window growth, identity, domains, program data, cancellation, budget)
and Highlight Roll-off with its formula. The recipe test compares Physical (with and without a key
and Mask, a partial scatter, the inverse cube), Streak (four-point star, chromatic anamorphic
streak, an odd star) and the shared controls in all three modes with an independent
double-precision reference; checks Physical's energy balance at intensities 0.4, 1 and 2.5; and
covers Streak's warm reuse, invalidation, ROI, cancellation and budget refusal. Compiler tests prove
a pre-#506 Glow lowers exactly as a default Classic one and check the Physical, Streak and shared
control chains. The native image-effect harness runs each mode and the shared controls on the
device against the CPU oracle at the 2e-6/exact-alpha gate with warm zero-dispatch and changed-value
runs.

## Inner Shadow and Inner Glow

`bloom.inner-shadow` and `bloom.inner-glow`, schema 1 (#528), are compositing recipes like Drop Shadow
and Glow, listed under Light & Shadow in the Add menu. Each has a required Image input, `input`, and
an Image output, `image`, takes no effect mask, and has no pixel kernel of its own: the compiler
lowers each to a chain of existing primitives (`appendInnerShadow`, `appendInnerGlow`), so every
operation keeps its CPU reference, resident GPU command, cache identity, ROI, budget, cancellation
and retirement. They paint inside the input's alpha and never change it: the output alpha is the
input's alpha, the paint is composited Atop the input, and no colour lies outside the input's alpha.
Both are premultiplication-correct and HDR-safe (colour is scaled straight, re-associated by the
primitive that owns it, and never clamped). The data window is the input's. The edge of the input's
data window is an edge like any other (the blur has a transparent edge), as for Light Wrap.

Inner Shadow parameters and defaults: `color` (black, opaque; its alpha scales the opacity),
`opacity` 0.75 in `[0, 1]`, `direction` 135 degrees measured clockwise from up, `distance` 5 pixels
in `[0, 16384]`, `softness` (shown as **Blur**) 5 pixels in `[0, 256]`, `choke` 0 in `[0, 1]`, and the
constant switch `shadowOnly` (off). Inner Glow: `color` (white, opaque), `opacity` 0.75, `intensity`
1 in `[0, 1000]` (a gain on the colour, so a glow can exceed white), `size` 10 pixels in `[0, 256]`,
`choke` 0, the constant choice `source` (Edge, the default, or Center) and the constant switch
`glowOnly` (off). Every pixel length is a full-resolution pixel count, so it scales with the render
scale. Everything except `source` and the switches is animatable.

The chain, with `a` the input's alpha:

1. Matte Blur coverage of `a` by `softness` (Inner Glow: `size`), Gaussian with a transparent edge: `m`.
2. Inner Shadow only: Transform 2D translating `m` by `(distance sin(direction),
   -distance cos(direction))` (bilinear), exactly as Drop Shadow's offset.
3. A white image with the input's window (Channel Select of ones), held out by `m`: Multiply Alpha
   with the invert switch. Its alpha `h = 1 - m` is the outside's reach into the pixel, 1 beyond the
   blur, and exactly 1/2 at a hard edge with no offset.
4. Levels on the alpha: input white `w = max((1 - choke) / 2, 1/1024)`, so the shaped coverage is
   `e = clamp(h / w, 0, 1)`. At choke 0 the edge itself is at full strength and fades inward over the
   blur (about three times `softness`); raising the choke lowers `w`, which widens the solid band
   before the fade, and at choke 1 anything the blur reaches is solid, a hard edge to within 1/1024.
   A blur of 0 gives a hard edge whatever the choke.
5. Levels colours it: the straight colour is `color` (Inner Glow: `color * intensity`), the alpha is
   `opacity * color.alpha * e`. Inner Glow with source Center swaps the alpha's output black and white
   so the alpha is `opacity * color.alpha * (1 - e)`: nothing at the edge, full deep inside. Edge and
   Center add up to the input's alpha when `intensity`, `opacity` and the colour's alpha are 1.
6. A Merge with the explicit Atop mode composites the paint onto the input (`co = cp af + ci (1 - ap)`,
   `ao = af`). Shadow Only and Glow Only use the In mode (`co = cp af`, `ao = ap af`) and publish the
   paint alone, clipped to the input's alpha.

A changed opacity, colour or intensity re-runs the colouring Levels and the Merge; a changed choke
also re-runs the shaping Levels; a changed distance, softness or size re-runs from the blur or the
offset on. A muted node passes its input through. The only wide operation is the one blur (two
separable passes whose support grows with the blur radius); every other operation is a single pass.

**Widgets.** Colour is a swatch that opens the colour picker (on the card and in Properties), and
Inner Shadow's direction is an angle dial beside its Distance, Blur, Choke and Opacity sliders.
Inner Glow's Source is a labelled two-segment radio, Edge or Center (`PropertiesRowControl::Radio`,
and the same `KRadioGroup` on the card), with Size, Choke, Intensity and Opacity sliders; every
edit is a command, so it undoes and animates like any parameter.

**Evidence.** The direct recipe test compares each chain and its variants (Edge and Center sources,
Only switches, choke 0, 0.5 and 1, blur 0, a solid input) with an independent double-precision
reference (a direct two-dimensional Gaussian, the hold-out, the divide and clamp, and the Porter-Duff
composite) at 1e-4. It also checks, at full and half render scale and on an HDR colour, that the
output alpha equals the input alpha, that no colour lies outside the alpha, that opacity 0 is the
input, that choke 1 hardens the edge, that Center plus Edge is the input's alpha, the warm reuse
and the operations a changed opacity, colour or choke re-runs, and the ROI. Compiler tests read the
operations and the parameter identities they use; registry, command, Properties and card tests cover
the definitions, refusals, undo/redo, the dial, the swatch and the radio. `node.bloom.inner-shadow`
and `node.bloom.inner-glow` prepare through the production builder (full frame and ROI), and the
native harness holds four variants to the CPU oracle at the 2e-6/exact-alpha gate with warm reuse
and a changed-value run. [Benchmarks](../development/benchmarks.md) lists `inner-shadow-2k` and
`inner-glow-2k`, written and not yet run.

## Alpha Association

`bloom.alpha-association`, schema 1 (#503), converts between the ways an image's colour relates to
its alpha. It is listed under Compositing (beside Set Alpha and Multiply Alpha; search also finds
it as Unmult, Premultiply, Unpremultiply or Remove Color Matting) and has one Image in, `input`, and
one Image out, `image`, over the input's data and display windows with its pixel aspect.

Parameters:

| Control | Role | Default | Kind |
| --- | --- | --- | --- |
| Mode | `mode` | Unmult | Constant dropdown: Unmult (0), Premultiply (1), Unpremultiply (2), Remove Color Matting (3) |
| White background | `white` | Off | Constant switch, Unmult only |
| Matte color | `background` | Black (0, 0, 0) | Animatable colour with a Color socket, Remove Color Matting only; its alpha is unused |

The mode and switch choose the arithmetic, so they have no socket. The matte colour's RGB is any
finite value within `+-1e6` (HDR and signed colours are allowed).

Bloom's process image is premultiplied (associated) scene-linear RGBA with alpha in `[0, 1]`. Every
mode works on the stored values with no colour transform and no clamp of colour. For an input pixel
`(r, g, b, a)`:

- **Unmult** derives alpha for footage shot on black (fire, smoke, flares, lens elements). The image
  over black is its premultiplied colour, so `a' = clamp(max(r, g, b), 0, 1)` and the colour is
  unchanged: the result composited over black is exactly the input over black, and over any other
  background it screens in as the footage's own light. HDR colour above one gives an opaque pixel
  whose colour stays above one; it is never clipped. With **White background** on, the image over
  white is `w = c + (1 - a)`, so `a' = clamp(1 - min(w), 0, 1)` and the colour is
  `w - (1 - a')`: the result over white is the input over white.
- **Premultiply** treats the colour as straight: `rgb * a`, alpha unchanged.
- **Unpremultiply** returns straight colour: `rgb / a`, alpha unchanged, for `a` above the epsilon
  `2^-20` (about 9.5e-7, exact in binary32). At or below it the colour passes unchanged and is never
  divided, so a near-zero alpha cannot amplify noise into huge values. A quotient that cannot be
  represented in binary32 (an RGB above about 3e32 over a small alpha) is reported as the grades'
  non-finite pixel failure, never published as an infinity. Premultiply after Unpremultiply returns
  the input (within rounding) wherever alpha is above the epsilon.
- **Remove Color Matting** recovers footage premultiplied against `background` instead of black.
  Such a pixel stores `aF + (1 - a)B`; the straight colour is `(c - (1 - a)B) / a`, so the
  associated result is `c - (1 - a)B` with alpha unchanged. No division is needed, so no epsilon
  applies. Against black it is the exact identity and aliases its input. Import such footage as
  premultiplied so its stored colour is the matted colour.

Alpha 0 is always transparent black (the storage invariant). Alpha is the input or a clamped
maximum, so it is exact, except over white, where it is computed from the image over white in
binary32 (the same operations on the CPU and the device, so they still agree exactly, within
binary32 rounding of the exact value). A transparent pixel stays transparent in every mode, so the
data window is unchanged. Unmult cannot keep colour where the alpha it derives is 0 (an
all-negative pixel over black, or one at or above white everywhere over white): such a pixel
becomes transparent black, as the process image cannot store colour at zero alpha.

The node lowers to one `AlphaAssociationKernel` (Required in the GPU coverage contract), a grade
kind of the shared colour grade machinery (`color_grade_image_effect.hpp`): the host prepares one
packed table `[table mode, epsilon, 0, 0] [background r, g, b, 0]`, the CPU reference applies it per
pixel, and the device runs one fixed resident program that reads the same table, with the same
binary32 operations (precise sums, fused multiply-adds, the wrapper's premultiply and correctly
rounded division). A changed mode, switch or colour changes only data, never the shader. The
background is lowered only in Remove Color Matting; the other modes lower a constant black without a
parameter identity, so an unused (even animated) colour neither claims the parameter nor
invalidates a cached frame. Cache identity is the prepared table; an animated matte colour makes
the operation time-dependent. Cancellation is checked per row, the output is charged to the pixel
budget, and preview, RAM preview, still and sequence export, video export and headless render all
take the same resident command; there is no exception.

Tests: the alpha association test compares every mode with an independent double-precision formula
over HDR, signed, partial, zero, near-zero and exactly-epsilon alpha (2e-6, with exact alpha
except over white);
checks Unmult over black and white, Unpremultiply then Premultiply, Remove Color Matting of a matted
pixel, the epsilon boundary and the overflow refusal; and covers identity, domains, the fixed
program, cache identity, the budget and cancellation. The image effect test runs the modes through
the evaluator (cold, warm reuse, ROI, invalidation) and samples an animated matte colour. The
compiler test checks the lowering. The `effect.AlphaAssociationKernel` and
`node.bloom.alpha-association` coverage fixtures prepare every mode on the production builder, and
the native grade harness runs every mode on the device against the CPU oracle at the 2e-6/exact-alpha
gate with a warm zero-dispatch rerun and a changed value that re-dispatches.

## Colorize

`bloom.colorize`, schema 1 (#504), replaces an image's colour from its own tones. It is listed under
Grade (search also finds it as fill, tint, tritone, duotone, gradient map, ramp, black white,
monochrome, greyscale, toner or channel mixer) and has one Image in, `input`, and one Image out,
`image`, over the input's data and display windows with its pixel aspect.

Parameters:

| Control | Role | Default | Kind |
| --- | --- | --- | --- |
| Mode | `mode` | Tint | Constant dropdown: Fill (0), Tint (1), Tritone (2), Gradient Map (3), Black & White (4) |
| Amount | `amount` | 100% | Animatable `[0, 1]`, every mode |
| Fill color | `fillColor` | White (1, 1, 1) | Animatable colour, Fill |
| Shadows | `shadows` | Black (0, 0, 0) | Animatable colour, Tint and Tritone (and an unbound Gradient Map) |
| Midtones | `midtones` | (0.24, 0.17, 0.10), luma 0.18 | Animatable colour, Tritone (and an unbound Gradient Map) |
| Highlights | `highlights` | White (1, 1, 1) | Animatable colour, Tint and Tritone (and an unbound Gradient Map) |
| Midpoint | `midpoint` | 0.18 (scene-linear mid grey) | Animatable `[0.01, 0.99]`, Tritone (and an unbound Gradient Map) |
| Gradient | `gradient` | Black at 0, Midtones' default at 0.18, white at 1 | Optional, animatable gradient (1 to 64 stops; document 1.38), Gradient Map |
| Map from | `source` | Luma | Constant dropdown: Luma (0), Red (1), Green (2), Blue (3), Alpha (4); Gradient Map |
| Outside the ramp | `cycle` | Clamp | Constant dropdown: Clamp (0), Repeat (1), Mirror (2); Gradient Map |
| Repetitions | `repetitions` | 1 | Animatable `[0, 100]`, Gradient Map |
| Phase (cycles) | `phase` | 0 | Animatable `[-1000, 1000]`, Gradient Map |
| Reds, Yellows, Greens, Cyans, Blues, Magentas | `reds` ... `magentas` | 40%, 60%, 40%, 60%, 20%, 80% | Animatable `[-2, 3]` (-200% to 300%), Black & White |

The three choices decide the arithmetic, so they have no socket, and neither has the gradient (a
whole value no value graph produces); every other value is animatable and drivable with its own
socket in parameter order. The gradient is optional (#541): a new node binds it, and a node saved
before document 1.38 has no binding and maps through its three-colour ramp until the gradient is first
edited, which binds it seeded with that ramp (see project-format.md, "Gradient Values In Document
1.38"). A colour's RGB is any finite value within `+-1e6`
(HDR and signed colours are allowed); its alpha is unused.

Bloom's process image is premultiplied scene-linear RGBA. Colorize works on the straight colour
`s = rgb / a` as stored, with no colour transform and no clamp of colour, and keeps alpha exactly; a
pixel with alpha 0 passes unchanged. `L = 0.2126 s.r + 0.7152 s.g + 0.0722 s.b` is the Rec. 709 luma
the track-matte Luma and Luma Key use. Each mode gives a colour `c`:

- **Fill**: `c = fillColor`, a flat colour over the kept alpha.
- **Tint**: `c = shadows + L (highlights - shadows)`: black maps to Shadows, white to Highlights,
  and the line continues past them, so HDR luma above one stays brighter than white.
- **Tritone** and **Gradient Map** map a position `t` through the three-stop ramp Shadows (at 0),
  Midtones (at the midpoint `m`) and Highlights (at 1), linear between stops in scene-linear
  colour: `c = shadows + (t / m)(midtones - shadows)` for `t <= m`, otherwise
  `c = midtones + ((t - m) / (1 - m))(highlights - midtones)`. Tritone takes `t = clamp(L, 0, 1)`.
  Gradient Map reads `x`, the luma or one straight channel (alpha is the pixel's alpha), takes
  `t = x * repetitions + phase`, and then clamps it to `[0, 1]` (Clamp), keeps its fractional part
  `t - floor(t)` (Repeat, a sawtooth) or reflects it, `u = t - 2 floor(t / 2)`, `t = u > 1 ? 2 - u : u`
  (Mirror, so neighbouring repetitions meet without a seam). Repeat or Mirror with several
  repetitions and an animated phase gives the cycling colour of a colour-cycle effect. With its
  gradient bound (#541), Gradient Map maps that `t` through the gradient's stops `(p_i, c_i)` ordered
  by position instead of the three-colour ramp: `c = c_0` for `t <= p_0`, `c = c_last` for
  `t >= p_last`, and otherwise `c = c_i + ((t - p_i) / (p_{i+1} - p_i))(c_{i+1} - c_i)` for the last
  stop with `p_i <= t` (two stops at one position make a hard edge: the later one wins from there
  on). A stop's alpha is kept in the gradient but unused: Colorize keeps the pixel's alpha. A stop a
  value node drives (#543) takes its driven colour or position at the frame before the stops are
  ordered, on the CPU reference and in the stop table the fixed program reads alike. That is the
  Linear RGB blend; a gradient in another interpolation or in HSV or HSL (#546) blends as
  evaluation-primitives.md's "Gradient Interpolation And Colour Modes" defines, the fixed program's
  table carrying the mode words and the spline modes' outer spans where the unused ramp colours sit.
- **Black & White** takes the largest, middle and smallest channel of `s` and
  `grey = min + (mid - min) * secondary + (max - mid) * primary`, `c = (grey, grey, grey)`. The
  primary weight belongs to the largest channel (Reds, Greens or Blues) and the secondary to the pair
  of the two largest, named by the smallest channel (Cyans when red is smallest, Magentas when green
  is, Yellows when blue is). White stays white, a pure red is Reds, a pure yellow Yellows, and so on;
  the defaults are the classic black-and-white mix. Ties go to the earlier channel (red, then green),
  and a tie contributes nothing whichever weight it picks.

Finally `c' = s + amount (c - s)` (exactly `c` at 100%) and the result is `(c' a, a)`. Amount 0 is
the exact identity and aliases its input. A non-finite straight colour, Gradient Map position or
result is the grades' non-finite pixel failure, never published.

The node lowers to one `ColorizeKernel` (Required in the GPU coverage contract), a grade kind of the
shared colour grade machinery (`color_grade_image_effect.hpp`): the host prepares one packed table of
ten vec4s (`[table mode, amount, source, cycle]`, `[repetitions, phase, midpoint, 0]`,
`[1 / m, 1 / (1 - m), 0, 0]`, the luma weights, the colours `A`, `B`, `C`, `D` as the mode needs
them (Fill `A`; Tint `A` = shadows, `B` = highlights - shadows; the ramp `A` = shadows,
`B` = midtones - shadows, `C` = midtones, `D` = highlights - midtones), and the weights by largest and
by smallest channel), the CPU reference applies it per pixel, and the device runs one fixed resident
program that reads the same table with the same binary32 operations. A bound Gradient Map is table
mode 4: the same ten vec4s (the ramp's colours unused) followed by two vec4s per stop in position
order, `[p_i, c_i]` and `[1 / (p_{i+1} - p_i) (0 for the last stop or a hard edge), c_{i+1} - c_i]`,
with the stop count in the table's eighth float; the table grows with the stops, and the same
fixed program walks them (at most 64) (the wrapper's correctly rounded
division and premultiply, precise fused multiply-adds, exact min, max and floor, the same channel
ties). A changed mode, colour, stop or value changes only data, never the shader. The compiler lowers
only the operands the mode reads (Gradient Map also its source and cycle, and its gradient when
bound, as one inline operand whose animation curve is compiled into the kernel like a Curves path);
the others are constants
without a parameter identity, and the table holds zero for them, so an unused (even animated) value
neither claims the parameter nor invalidates a cached frame. Cache identity is the prepared table; an
animated value the mode reads makes the operation time-dependent. Cancellation is checked per row,
the output is charged to the pixel budget, and preview, RAM preview, still and sequence export, video
export and headless render all take the same resident command; there is no exception.

The card widget (`colorize_widget.hpp`), drawn the same on the node card and in Properties, shows
the mode's controls and keeps the rest behind it: Amount over its slider; Fill's colour chip; for
Tint and Tritone the ramp bar, the gradient drawn from the node's own colours with a stop under it
per colour (dragging the midtones stop moves the midpoint, relatively, with Ctrl a tenth of the
speed; a Shift-press on the bar jumps it to the pointer; clicking a stop opens its colour picker; a
double-click resets the midpoint), each stop's colour row (a chip that opens the colour picker) and
Tritone's Midpoint row; Gradient Map (#541) the gradient in the kit gradient editor
(`kit::KGradientEditor`: drag a stop along the bar, press the bar to add one, pull one off the bar to
remove it, double-click a stop for its colour picker, Distribute Evenly and Reverse on its menu) with
the gradient's keyframe diamond, the selected stop's colour chip and position (%), then the
Repetitions and Phase sliders and its two choices as rows; Black & White shows the channel mix, one slider row per hue over a track from black
to that hue. Each value keeps its diamond, its Reset and key menu and its socket beside its row.

Tests: the colorize test compares every mode, each Gradient Map source and cycle, a partial amount
and a custom mix with an independent double-precision formula over HDR, signed, partial and zero
alpha (2e-5 relative, exact alpha); checks Fill's flat colour, Tint's and Tritone's stops, the
classic grey weights and the periodic and mirrored maps; and covers identity, unused values,
domains, the fixed program, cache identity, the overflow refusal, the budget and cancellation. The
image effect test runs modes through the evaluator (cold, warm reuse, ROI, invalidation); the
compiler test checks the lowering of each mode; the commands test edits and refuses values. The
`effect.ColorizeKernel` and `node.bloom.colorize` coverage fixtures prepare every mode on the
production builder, and the native grade harness runs every mode on the device against the CPU
oracle at the 2e-6/exact-alpha gate with a warm zero-dispatch rerun and a changed amount that
re-dispatches; the coverage and native fixtures also run a bound Gradient Map (several stops, a hard
edge, Repeat). The colorize test checks the mode-4 table against an independent stop walk, unsorted
stops, a hard edge, one stop, the 64-stop limit and a refused malformed table. The grading cards test
holds the card in every mode inside its card with no label cut short, the ramp drag as one command,
the stop click opening the picker, and the gradient editor's stop drag, add, pull-off removal and
position field as one command each, keyed as one whole value whose keys interpolate.

## Vignette

`bloom.vignette`, schema 1 (#505), darkens (or brightens) an image towards the edges of its frame,
the finishing step of a lens look. It is listed under Stylize (search also finds it as lens, falloff,
darken edges or corners) and has one Image in, `input`, and one Image out, `image`, over the input's
data and display windows with its pixel aspect.

Parameters:

| Control | Role | Default | Kind |
| --- | --- | --- | --- |
| Shape | `shape` | Elliptical | Constant dropdown: Elliptical (0), Rectangular (1) |
| Mode | `mode` | Multiply | Constant dropdown: Multiply (0), Exposure (stops) (1) |
| Amount | `amount` | 50% | Animatable `[-1, 1]`, Multiply |
| Exposure | `exposure` | -1 stop | Animatable `[-10, 10]` stops, Exposure |
| Midpoint | `midpoint` | 50% | Animatable `[0, 1]` |
| Roundness | `roundness` | 0% | Animatable `[-1, 1]` |
| Feather | `feather` | 50% | Animatable `[0, 1]` |
| Highlight protection | `highlightProtection` | 0% | Animatable `[0, 1]` |
| Center | `center` | (0, 0) | Animatable Vector 2, pixels from the frame's centre, each axis within `+-1e6` |

The two choices decide the arithmetic, so they have no socket; every other value is animatable and
drivable with its own socket in parameter order (the centre's a Vector 2).

The vignette is measured against the input's display window (the frame it belongs to, not its data
window), so a data window grown by a blur or cut by a region of interest changes nothing, and in
full-resolution units, so a proxy resolution draws the same vignette. With the display window's
width `W` and height `H` in full-resolution pixels, the pixel aspect `PAR` and the centre
`C = (W / 2, H / 2) + center`, a pixel centre `p` (in full-resolution pixels) sits at
`q = ((p.x - C.x) PAR / A.x, (p.y - C.y) / A.y)`, where the half-axes `A` are the frame's halves
`(W PAR / 2, H / 2)` moved geometrically towards their geometric mean (a circle of the same area)
by `circle = max(roundness, 0)`: `A.x = (W PAR / 2)^(1 - circle) * g^circle` and likewise for `y`,
`g = sqrt(W PAR / 2 * H / 2)`. With roundness 0 the shape touches the frame's edges; at 1 it is a
circle (a square for Rectangular) whatever the frame's aspect.

The distance is a superellipse norm, `d = (|q.x|^n + |q.y|^n)^(1/n)`, each `|q|` first limited to 4
(beyond that every falloff is complete): `n = 2` (an ellipse) for Elliptical and `n = 6` (a rounded
rectangle) for Rectangular, multiplied by `4^(-roundness)` when roundness is negative, which squares
the corners off. The falloff is centred at `c = 0.4 + 1.2 midpoint` and runs from
`inner = c (1 - feather)` to `outer = c (1 + feather)`: `t = clamp((d - inner) / (outer - inner), 0, 1)`
and `mask = t^2 (3 - 2t)`, or a hard edge, `mask = d >= inner ? 1 : 0`, with feather 0. With the
defaults the frame's edges sit at half strength and its corners at nearly full strength.

The gain is `1 - amount * mask` (Multiply; a negative amount brightens) or `2^(exposure * mask)`
(Exposure, in stops). Highlight protection moves it back towards one for bright pixels:
`h = smoothstep((L - 0.18) / 0.82)` of the straight Rec. 709 luma `L` (the track-matte Luma's
weights), and `gain' = gain + protection * h * (1 - gain)`, so mid grey and below take the whole
vignette and a pixel at 1.0 or brighter keeps that share of its exposure. The premultiplied colour is
multiplied by the gain and alpha is kept exactly; a pixel with alpha 0 passes unchanged. No colour
transform is applied: the arithmetic is scene-linear, on the values as stored. An amount (or an
exposure) of 0 is the exact identity and aliases its input. A non-finite result is the grades'
non-finite pixel failure, never published.

The node lowers to one `VignetteKernel` (Required in the GPU coverage contract), a grade kind of the
shared colour grade machinery (`color_grade_image_effect.hpp`, `vignette_image_effect.cpp`): the
host prepares a table of five vec4s from the sampled values (`[mode, strength, protection, hard edge]`,
`[inner, 1 / (outer - inner), n, 1 / n]`, the luma weights with an ellipse flag, the protection's
`[0.18, 1 / 0.82, circle, 0]` and the centre offset), and where the effect is applied (the CPU
evaluator from its input image, the GPU scene builder from its input command's descriptor) appends
the input's geometry as a sixth: the centre and the per-axis scales in the data window's own pixel
indices, from its display window, data origin, pixel aspect and the evaluation scale. The CPU
reference reads those 24 floats per pixel, and the device runs one fixed resident neighbourhood
program over the same floats with the same binary32 operations (precise fused multiply-adds, the
wrapper's correctly rounded division; `sqrt`, `pow` and `exp2` to the device's precision). A changed
value changes only data, never the shader. The compiler lowers only the strength the mode reads (the
other is a constant without a parameter identity, and the table holds only the one read), so an
unused, even animated, strength neither claims the parameter nor invalidates a cached frame. Cache
identity is the prepared table (the frame key already carries the resolution and region of
interest); an animated value makes the operation time-dependent. Cancellation is checked per row,
the output is charged to the pixel budget, and preview, RAM preview, still and sequence export,
video export and headless render all take the same resident command; there is no exception.

The card widget (`vignette_widget.hpp`), drawn the same on the node card and in Properties, puts the
centre on a point pad (`kit::KPointPad`) over a picture of the frame that shows the falloff as the
node computes it (shape, roundness, midpoint, feather and the strength over mid grey), so dragging
the centre or any slider redraws where the darkening falls; the centre keeps its X/Y row, diamond and
socket among the node's rows. Under the pad are slider rows for Amount (Multiply) or Exposure
(Exposure), Midpoint, Roundness, Feather and Highlight protection, each with its diamond, its Reset
and key menu and its socket beside it. Shape and Mode are the node's dropdown rows.

Tests: the vignette test compares both shapes and modes, roundness both ways, a moved centre, a hard
edge, highlight protection and anamorphic pixels with an independent double-precision formula over
HDR, signed, partial and zero alpha with a data window that overhangs its display window (2e-5
relative, exact alpha); checks the untouched centre, black and exactly-the-stops corners, the circle
on a wide frame, protected highlights, the rectangle's reach and a proxy resolution's agreement; and
covers identity, unused strengths, domains, the fixed program, the geometry refusals, cache identity,
the budget and cancellation. The image effect test runs both modes through the evaluator (cold, warm
reuse, ROI, invalidation by a moved centre and a changed mode); the compiler test checks the lowering
of each mode and a muted node; the commands test edits and refuses values. The
`effect.VignetteKernel` and `node.bloom.vignette` coverage fixtures prepare both shapes and modes on
the production builder, full frame and ROI, and the native grade harness runs them on the device
against the CPU oracle at the 2e-6/exact-alpha gate with a warm zero-dispatch rerun and a moved
centre that re-dispatches. The grading cards test holds the card in both modes and shapes inside its
card with no label cut short, the falloff drawn on the pad and following the centre, and the centre
drag and a slider drag as one command each.

## Motion blur

Motion Blur, Vector Motion Blur, Optical Flow, Optical Flow Motion Blur, Echo, and Directional, Spin
and Zoom Blur are one temporal-sampling engine, specified in [Motion blur](motion-blur.md).

## Depth effects

`bloom.z-defocus` (Z Defocus) and `bloom.depth-fog` (Depth Fog), schema 1 (#483), are the two
nodes that read a depth pass: a lens blur whose size follows the depth, and atmospheric fog
that thickens with distance. Both take an Image on `input`, a depth source on `depth`, an optional
Image (`bokeh`, the lens kernel; `fog`, a fog picture) and the effect mask (Matte Workflow), and give
an Image on `image`. In a comp, fog normally goes before defocus: fog is what the far objects look
like, and the lens then blurs the fogged picture, as a camera would.

### Research basis

The algorithms follow published techniques and the behaviour of established compositing tools,
not new inventions:

- **Thin-lens circle of confusion** (Potmesil and Chakravarty, "A Lens and Aperture Camera Model
  for Synthetic Image Generation", SIGGRAPH 1981): for focal length `f`, f-number `N` and focus
  distance `S`, a point at distance `d` images as a disk of diameter
  `c = f^2 / (N (S - f)) * |d - S| / d` on the sensor. The Physical lens mode is exactly this,
  scaled from sensor millimetres to pixels by the frame width over the sensor width.
- **Scatter-as-gather with a tile maximum and layered near/far handling** (GPU Gems 3, chapter 28,
  Earl Hammon, "Practical Post-Process Depth of Field"; Jorge Jimenez, "Next Generation Post
  Processing in Call of Duty: Advanced Warfare", SIGGRAPH 2014 Advances in Real-Time Rendering;
  Kyle McIntosh, "Efficiently Simulating the Bokeh of Polygonal Apertures in a Post-Process Depth of
  Field Shader", 2012): every output pixel gathers the samples whose own circles reach it, sized by
  a dilated tile maximum of the circles so a large foreground blur is found by the pixels it covers,
  with the near (foreground) and far (background) layers weighed separately and combined by the
  foreground's coverage, so a blurred foreground spreads over a sharp background but a sharp
  foreground never takes the background's colour.
- **Depth-of-field compositing nodes** in node-based compositors and an open-source 3D suite's
  Defocus node, and the commercial lens-blur plug-ins: focus distance and depth of field, a maximum
  blur, near/far-only blur, size from a size map, a uniform (no depth) blur, polygonal bokeh with
  blade rotation and curvature, an image as the lens kernel, highlight boost and chromatic bokeh,
  and an edge expansion of the near layer for non-antialiased depth.
- **Fog**: the OpenGL fixed-function fog equations (linear `(z - start) / (end - start)`,
  exponential `1 - exp(-density z)` and exponential squared `1 - exp(-(density z)^2)`), the
  exponential being Beer-Lambert extinction through a uniform medium; a renderer's mist pass with
  its linear, quadratic and inverse-quadratic falloffs.
- **Deep compositing** is the complete answer to depth-of-field and fog edges (each sample keeps its
  own depth); a flat depth pass keeps one depth per pixel, which is why the near layer is expanded
  and empty depth has an explicit rule below.

### The depth source

The depth is any pixel input: an OpenEXR Z or mist pass through Pass or Shuffle (or the channel set
directly, the channel picked by name), a PNG, TIFF or JPEG depth or mist map (8 or 16-bit integer,
or float), or any node's image. `depthChannel` picks one channel: R, G, B or A of an Image or Mask
(empty is the first channel, R; for a Mask, its coverage), or a channel set's channel by name
(empty is the set's first data channel). It is never a luminance mix of the colour channels. An
unknown name on an Image is a compile failure that says so. With no depth connected the input
passes through (outside Uniform).

The stored value is read straight (colour divided by alpha; alpha read as is) and interpreted by
`interpretation`: **Z** (the distance in scene units), **Inverse depth (1/Z)**, **Normalised (0-1)**
(0 at `near`, 1 at `far`, linear) or **Mist (0-1)** (0 at `near`, 1 at `far` after the renderer's
falloff, `mistFalloff` Linear, Quadratic or Inverse quadratic, which is undone first: `sqrt` of a
quadratic mist, the square of an inverse-quadratic one). `invert` reads a normalised or mist map
with white near. A pixel is **empty** (no surface) when its alpha is 0, its value is not finite,
or (Z) it is not above 0 or is 1e9 or more, or (1/Z) it is not above 0; `background` says what an
empty pixel is: infinitely far (the default), at the far distance, or unaffected (no blur, no fog).

Integer files arrive as their normalised value, so an 8-bit depth has only 256 steps across its
whole range: blur sizes and fog band visibly on a slow gradient. Use 16-bit or float depth; an 8-bit
map is fine only for a short range or a coarse effect.

A depth pass is data. The card warns when the depth comes from an Image or Video source read through
a colour transform (sRGB, Linear, Automatic or an OCIO input space, followed through Reroutes and
the channel nodes that keep values) and offers **Read as data**, which sets that source's colour
space to Raw and clears its input colour space in one command. It also warns when the connected
image looks like a colour picture: on a worker, at a small proxy of the display route, the share of
covered pixels whose red, green and blue differ by more than 24 levels of 255 is measured
(`ImagePreviewController::depthChannelSpread`), and above 5% the card says a beauty or normal pass
seems connected. A grey depth pass spreads by none. Neither check touches the render.

### Z Defocus

| Control | Role | Default | Kind |
| --- | --- | --- | --- |
| Mode | `mode` | Depth | Depth (0), Physical lens (1), Size map (2), Uniform (3) |
| Depth channel, Depth is, Near/Far distance, Near is white, Mist falloff, Empty depth | as above | first channel, Z, 0.1 / 100, off, Linear, Infinitely far | Constants and animatable distances |
| Scene units | `units` | Metres | Metres, Centimetres, Millimetres, Feet, Inches (Physical) |
| Focus from | `focusFrom` | Focus distance | Focus distance (0), Depth at the focus point (1) |
| Focus distance | `focusDistance` | 10 | Animatable, scene units |
| Focus point | `focusPoint` | (0, 0) | Animatable Vector 2, pixels from the frame's centre |
| In-focus range | `depthOfField` | 0 | Animatable, scene units (Depth) |
| Blur size | `size` | 16 px | Animatable `[0, 256]` (Depth, Size map, Uniform) |
| Maximum blur | `maxSize` | 64 px | Animatable `[0, 256]` (Depth, Physical) |
| Focal length, Sensor width, F-stop | `focalLength`, `sensorWidth`, `fStop` | 50 mm, 36 mm, 2.8 | Animatable (Physical) |
| Blur | `blurRegion` | Near and far | Near and far, Far only, Near only |
| Edge expand | `edgeExpand` | 1 px | Constant `[0, 4]` |
| Bokeh shape | `shape` | Disk | Disk, Polygon (blades), Image |
| Blades, Blade rotation, Blade curvature | `blades`, `rotation`, `roundness` | 6, 0 degrees, 0 | Constant `[3, 16]`, animatable |
| Highlight boost, Highlight threshold | `highlightGain`, `highlightThreshold` | 0, 1.0 | Animatable |
| Chromatic bokeh | `chromatic` | 0 | Animatable `[0, 1]` |
| Quality | `quality` | Normal | Draft, Normal, High, Maximum: 5, 8, 12 or 16 rings |

Every distance, size and lens value is animatable and drivable with its own socket (rack focus is
an animated focus distance or focus point); the choices, blades and edge expand are constants.

**The circle of confusion.** The depth map (`DepthMapKernel`, CircleOfConfusion) writes each
pixel's signed radius `c` in evaluation pixels, negative in front of the focus (near) and positive
behind it (far), clamped to the widest radius `R` (`maxSize`, or `size` for Size map and Uniform):

- Depth: with focus `S` and half the in-focus range `h`, `c = size (d - (S + h)) / d` behind and
  `-size ((S - h) - d) / d` in front, 0 within the range; infinitely far is `size`.
- Physical: `c = K (d - S) / d`, `K = f^2 / (N (S_mm - f)) * W / (2 sensor)` (`S_mm` the focus in
  millimetres by the scene units; `W` the frame width in pixels); infinitely far is `K`.
- Size map: `c = size * v`, the picked channel clamped to 0-1 (and inverted when asked).
- Uniform: no map; every pixel's radius is `size`.

Focus from the focus point reads the mean distance of the surface pixels in the 5 x 5 area around
the point (empty pixels skipped), so a pick is not thrown by one noisy or edge pixel. Near only and
Far only clamp `c` to one sign. **Edge expand** takes the most negative `c` within `edgeExpand`
pixels: a non-antialiased depth's foreground edge pixels (often holding background depth) blur with
the foreground instead of leaving a sharp fringe. The map's alpha is `|c| / R`, which a Dilate of
radius `min(R (1 + chromatic / 4), 256)` turns into the tile maximum. That Dilate carries each
pixel's own colour (its Rgba policy: alpha dilated, red, green and blue the centre pixel's, zero in
the grown halo), so one image holds both the signed `c` in red and the tile maximum in alpha.

**The gather** (`DefocusKernel`). Each output pixel `p` with radius `c_p` gathers over a ring
pattern sized by `R_p = max(|c_p|, tile R)`: `K` rings (the quality) at spacing
`s = max(1, R_p (1 + chromatic / 4) / K)`, ring `k` holding `8k` samples (odd rings offset by half a
step), each sample at the integer offset nearest its position. A sample `q` at distance `rho` with
radius `c_q` contributes:

- to the **foreground**, `phi = clamp(c_p - c_q + 0.5, 0, 1)` (`q` in front of `p`) times its disk's
  coverage `clamp((|c_q| - rho) / s + 0.5, 0, 1)`, divided by the share of the pattern's area its
  ring covers (a 1/8-step density table), so a foreground's spread energy is conserved;
- to the **background**, `1 - phi` times the coverage of `min(|c_q|, |c_p|)`: a background sample
  never reaches past the receiving pixel's own circle, so a sharp foreground keeps its edge (no
  bleeding of the background into it, no halo around it).

The output is `alpha F + (1 - alpha) B`, the means of the two layers combined by the foreground's
coverage `alpha = min(sum of foreground coverage, 1)`, its alpha from the unweighted coverage. A
Polygon shape scales each sample's distance by its direction's polygon radius (blades, rotation,
curvature toward the circle), so the bokeh is that polygon. An Image shape weighs each sample by
the kernel image's bilinear value at the sample's rotated position (the colour only, so a coloured
kernel tints the bokeh). Highlight boost multiplies a sample's colour weight by
`1 + gain clamp((L - threshold) / threshold, 0, 1)` (`L` its straight Rec. 709 luma), so bright
points bloom into bokeh disks; chromatic bokeh scales the red channel's circle by `1 + chromatic / 4`
and the blue's by `1 - chromatic / 5`. Outer rings read a box prefilter of the input (radius
`(R / K - 1) / 2`, the ring spacing at the widest blur, blended in by the spacing) so wide blurs do
not alias. Samples outside the map's window count as in focus. The output window is the input's
grown by the widest reach.

### Depth Fog

| Control | Role | Default | Kind |
| --- | --- | --- | --- |
| Depth channel, Depth is, Near/Far distance, Near is white, Mist falloff, Empty depth | as above | first channel, Mist, 0 / 1, off, Linear, Infinitely far | as above |
| Fog color | `color` | (0.75, 0.8, 0.85) | Animatable colour |
| Fog amount | `amount` | 1 | Animatable `[0, 1]` |
| Falloff | `falloff` | Linear | Linear, Quadratic, Inverse quadratic, Exponential, Exponential squared, Curve |
| Fog start, Fog end | `start`, `end` | 0, 1 | Animatable distances |
| Density | `density` | 1 | Animatable (exponential falloffs) |
| Falloff curve | `curve` | linear | Curves master point list (Curve) |
| Fog transparent areas | `fillEmpty` | off | Boolean |

The fog factor (`DepthMapKernel`, Fog) is `amount F`, with `t = clamp((d - start) / (end - start))`
and `F = t`, `t^2`, `sqrt(t)`, `1 - exp(-density z)`, `1 - exp(-(density z)^2)` (`z = max(d -
start, 0)`) or the curve's 257-entry table at `t`; an empty pixel is fully fogged (infinitely far),
at the far distance, or unfogged. The fog layer is the fog colour (a Colorize Fill) or the fog
image, over the input's coverage (or, with Fog transparent areas, over the whole depth window), and
the result is an Image Mix of the input toward it with the factor as its Mask.

### Evaluation, GPU and cost

Both depth stages and the gather are grade kinds of the shared colour grade machinery
(`depth_effect_image_effect.cpp`): the host prepares one packed binary32 table (the operands, the
ring pattern with its polygon radii and kernel positions, the density table, the fog curve), and
where the effect is applied the CPU evaluator and the GPU scene builder append the geometry (the
focus point's pixel, the windows, the evaluation scale). The CPU reference and the fixed resident
GPU programs run the same binary32 operations over the same data (precise fused multiply-adds, the
wrapper's correctly rounded division; `sqrt` and `exp` to the device's precision). On the GPU the
gather reads its images (the colour, the tile maximum carrying the map, the prefilter and the
kernel) from one atlas beside the output window; nothing is read back per operation. Every pass
over the atlas costs the whole atlas, so it is assembled with one pass a slot: the colour padded to
the atlas by one exact move, and each slot moved into place and blended over it in Normal mode (the
regions are disjoint, so every pixel is one image's exact value). The prefilter is left out when
the widest ring spacing is at most one pixel, where its mix is zero everywhere. The chains are
built by the shared recipes (`depth_effect_recipes.cpp`) from existing operations (Dilate, Box
blur, Colorize, Image Mix), so a changed lens value re-runs only the map and the gather. The radius
is bounded by 256 pixels like every neighbourhood filter, the operations are charged to the pixel
budget, cancellation is checked per row, and a region of interest gathers only what it needs.
`DepthMapKernel` and `DefocusKernel` are Required in the GPU coverage contract with the
`node.bloom.z-defocus` and `node.bloom.depth-fog` fixtures; the native harness holds both at the
2e-6 colour, exact alpha gate with warm zero-dispatch reuse. The cost per frame against a Grade and
a Gaussian blur is in [benchmarks](../development/benchmarks.md), "Depth effects".

Defaults chosen here: a Z interpretation for Z Defocus (a renderer's Z pass) and Mist for Depth Fog
(a mist pass is the common fog input); empty depth infinitely far (the sky is far and fogged);
edge expand 1 pixel; Normal quality (8 rings); a 64-pixel maximum blur.

The card (`depth_effect_widget.hpp`) shows the two depth checks, the near/far and fog start/end as
range rows over the depth's own units, the focus point on a point pad with **Pick focus in
Viewer**, the blade rotation on a dial and the fog curve on a curve graph; rows the current mode,
interpretation, shape or falloff does not read are hidden. **Pick focus in Viewer** arms the
Viewer's focus pick (`CompositionSession::requestFocusPick`): a click on the frame, mapped through
the world transform of the layer that displays the node, sets the focus point and focuses from it in
one command (a key at the playhead when the point is animated); Shift-click keeps picking for a
rack focus; Escape cancels. While a Z Defocus that focuses from a point is selected, the Viewer
draws its focus point and the 5 x 5 area it samples.

## Lens Flare

`bloom.lens-flare`, schema 1 (#507), listed under Light & Shadow, adds an element stack of light to an image:
an Image on `input`, an optional matte on `occlusion` (a Mask, or an Image read through the channel
selector) and an Image on `image`. It lowers to one `LensFlareKernel` (Required in the GPU coverage
contract), a grade kind whose table gains the image's geometry where it is applied. Its parameter
vocabulary is one table (`lens_flare_nodes.hpp`); the layout, presets and definition are shared with
the card's picture (`lens_flare_definition.hpp`). Each Scalar, Vector 2 and Colour is animatable and
drivable with its own socket; each Choice and count is a constant, because it chooses the program's
loops.

**Geometry.** Lengths are in full-resolution frame heights from the display window's centre, x right
and y down, with x scaled by the pixel aspect, so a flare keeps its shape at any proxy scale and
pixel aspect. The light is `L = position * s / H` with `H` the full-resolution frame height and
`s = 1`, or in the 3D mode `s = zoom / (zoom + distance)`; at `zoom + distance <= 0` the light is
behind the camera and the node is the identity. A tracked or projected light drives `lightPosition`
(and the distance and zoom) through its sockets: Bloom has no 3D camera or tracker node of its own
yet. The light's position, colour, size and rotation have roles of their own (`lightPosition`,
`lightColor`, `flareSize`, `flareRotation`), never `position`, `color`, `size` or `rotation`, which
the node card, Properties and the Viewer read as a layer's transform and a source's colour.

**Output.** The output window is the input's data window joined with its display window, so the light
reaches the whole frame from a smaller input. With `F` the stack's premultiplied light (each channel
at least 0) times the level below and `m = max(F.r, F.g, F.b)`:

```text
rgb' = rgb + F
a'   = a + (1 - a) * min(m, 1)
```

so an opaque image stays opaque and a transparent one takes the light's coverage. A sum with
`m < 2^-30` adds nothing, which keeps the tails exact where a device flushes subnormals. The node is
the identity at intensity 0, with no element on, or behind the camera.

**The stack.** With `d` a pixel's offset from the light and `c` its offset from the frame's centre,
`colour = color * intensity * amount` for each element (the anamorphic streak's own colour), and
`bump(t) = max(1 - t, 0)^2`. `split = 0.2 * chromatic` moves a channel's radius by `1 + split`
(red), `1` (green) or `1 - split` (blue):

| Element | Light per channel | Sizes |
|---|---|---|
| Glow | `0.625 bump(\|d\|^2 / (R/3)^2)^4 + 0.375 max(1 - \|d\|^2 / R^2, 0)^3` | `R = 3 size glowSize` |
| Streaks | sum over the lines of `max(1 - (\|d.v\| / l + \|d x v\| / w), 0)^3`, a needle narrowing to its tip | `l = size streakLength (0.7 to 1)`, `w = size streakWidth` |
| Ring | `bump(t^2)`, `t = (\|d\|^2 - r^2) / (2 r w)` | `r = size ringSize` times the split |
| Hoop | as the ring around `c`, times `s^2`, `s = clamp(0.5 + 0.5 (c.n) / r, 0, 1)` | `n` is the unit vector away from the light, fading to 0 within a quarter frame height of the centre |
| Anamorphic | `bump(\|d.v\| / l + \|d x v\| / w)` | `v` is the rotation |
| Ghosts | `clamp((1 - t) / 0.2, 0, 1) (0.5625 + 0.4375 t^2)`, `t = \|q\|^2 / r^2` | `q` the offset from the disc's centre |
| Iris | `clamp((1 - t) / 0.12, 0, 1) (0.625 + 0.375 t)`, `t = max_i(q.n_i) / r` | `n_i` the polygon's edge normals |

A ghost or iris sits at `L * a` on the axis through the light (`a = 1`) and the frame's centre
(`a = 0`), `a = spread (0.85 - 2.2 u)`, with radius `size * elementSize * (0.5 + v)` and a tint towards
one of four coating colours, `u`, `v` and the tint from a fixed sequence keyed by the pattern (`seed`)
and the disc's index, so the arrangement is part of the document (`lensFlareLayout`). The tables hold
8 streak lines, 16 ghosts, 12 irises and 12 blades. A shape outside its own reach skips its
arithmetic.

The ring and hoop are drawn from the squared distance: `(|d|^2 - r^2) / (2 r w)` is
`(|d| - r) / w` to first order, and the ring's width is small next to its radius. That, and polynomial
shapes that reach exactly 0, leaves additions, multiplications, abs, min and max as the only
operations, each exact on every device (the program marks them `precise`), so the CPU reference and
the GPU agree within 2e-6 on RGB with exact alpha, which is what the native harness holds them to.

**The level.** Two readings of the disc of `sampleRadius` pixels around the light (32 samples on a
sunflower spiral, whole-pixel offsets, the first at the light's own pixel; a sample outside its image
is transparent):

```text
gain    = 1 + sourceBrightness * (clamp(mean(luma) / sourceWhite, 0, 1) - 1)
covered = clamp(mean(coverage), 0, 1), or 1 - that when the selector inverts
visible = 1 - occlusionAmount * covered
level   = gain * visible
```

`luma` is Rec. 709 luma of the input's premultiplied colour. `coverage` is the premultiplied channel
the selector names, each sample clamped to 0 to 1; a Mask is lowered to its alpha, keeping the invert.
A level of 0 returns the input. Without a matte connected there is no occlusion.

**On the GPU.** The stack is one fixed resident program over the input padded to the output window.
Each reading is a second fixed program over the disc's window cut from its image (an exact move),
leaving the mean in that window's first pixel, so the disc is read once however large the frame; the
pixel is moved to a column beside the frame (the atlas), blended in, and the stack reads it there. A
disc outside its image reads the same 0 as a constant in the program data, without a pass. Without
readings there is no atlas: at most one exact pad and the program.

**Evidence.** `src/runtime/tests/lens_flare_tests.cpp` holds the glow, ring and ghost to independent
formulas, each promise above, the readings (white, share, each matte state, the disc's fade over an
edge), the projection, the domains, the fixed programs, the presets, the pixel budget and
cancellation. The native harness (`runLensFlares`) holds eight flares, every path of the program, to
the CPU oracle with exact alpha, warm RAM reuse with zero dispatches, and a changed value re-running
with parity. `effect.LensFlareKernel` and `node.bloom.lens-flare` prepare through the production
builder at full frame and ROI; the reading images take full support on the CPU, so a region of
interest draws the whole frame's pixels. A value change is new program data and a new cache identity.
[Benchmarks](../development/benchmarks.md#lens-flare-507) holds its cost.

## Film Grain

`bloom.film-grain`, schema 1, under Stylize (issue #486), has one Image in, `input`, one Image out,
`image`, and the optional effect mask every filter takes. Every Scalar below has a socket and can be
keyed or driven; `animation`, `seed`, `monochrome` and `mode` are constants:

| Role | Default | Domain | Meaning |
|---|---|---|---|
| `intensity` | 0.02 | 0 to 1 | The grain's standard deviation in linear light where the response is 1 |
| `size` | 1 | 0.25 to 32 | How wide a grain is, in full-resolution pixels |
| `redAmount`, `greenAmount`, `blueAmount` | 1 | 0 to 10 | Each channel's share of the intensity |
| `shadows`, `midtones`, `highlights` | 0.1, 1, 1 | 0 to 10 | The response at luma 0, 0.18 and 1 |
| `animation` | Every Frame | Every Frame (0), Held (1) | Whether each frame has its own grain. Constant |
| `seed` | 1 | 0 to 2^31 - 1 | Which grain. Constant |
| `monochrome` | off | off, on | One grain pattern shared by every channel. Constant and optional: a node saved before it opens with it off ([project format](project-format.md#film-grain-monochrome)) |
| `mode` | Film | Film (0), Sensor Noise (1) | Which model draws the grain (#565). Constant and optional: a node saved before it opens in Film ([project format](project-format.md#film-grain-sensor-noise)) |
| `shadowWeighting` | 1 | 0 to 10 | Sensor Noise: how strongly the noise favors the shadows. Optional |
| `chromaBlotches` | 0.5 | 0 to 10 | Sensor Noise: the strength of the low-frequency chroma blotches, relative to the intensity. Optional |
| `blotchSize` | 24 | 1 to 512 | Sensor Noise: how wide a blotch is, in full-resolution pixels. Optional |
| `fixedPattern` | 0 | 0 to 10 | Sensor Noise: the strength of the pattern that never changes from frame to frame, relative to the intensity; 0 is off. Optional |

Everything above this row's `mode` describes Film, the node as it always was. Sensor Noise is described
after the GPU paragraph below.

**The pixel.** With premultiplied `rgb` and `a`, `L = max(luma(rgb) / a, 0)` the pixel's straight
Rec. 709 luma, `n` the noise below (each channel's own, or with `monochrome` red's in all three) and
`k` each channel's amount:

```text
response(L) = shadows + (midtones - shadows) * L / 0.18          L < 0.18
            = midtones + (highlights - midtones) * (L - 0.18) / 0.82   0.18 <= L < 1
            = highlights * L                                     L >= 1
rgb' = rgb + a * response(L) * intensity * k * n
a'   = a
```

The grain is added in proportion to alpha, so the image stays premultiplied. Alpha 0 stays
transparent black. Nothing is clamped: an HDR value keeps its range, the last line of the response
keeps the grain the same share of a highlight above 1, and a dark value can go slightly negative
rather than lifting the grain's mean. The node is the identity at intensity 0, with every amount at 0
or with every response at 0.

**Monochrome.** Every channel reads red's noise, so `rgb' = rgb + a * response(L) * intensity * k * n_red`,
and `redAmount`, `greenAmount` and `blueAmount` keep scaling that one pattern. Equal amounts over a
neutral pixel stay neutral to the last bit; unequal amounts tint the grain but it stays one pattern, and
an amount of 0 keeps its channel exactly. Red is the pattern, so turning `monochrome` on leaves the red
channel's every bit as it was, and a seed's grain is the same grain. Each channel keeps a standard
deviation of `intensity * k`, which raises the deviation of the Rec. 709 luma at equal amounts from
about 0.75 to 1 of it: the channels no longer partly cancel. The option leaves the hash, the stream and
the seed alone, and with it off the node draws every bit it drew before it existed.

**The noise.** A pixel's centre `(x + 0.5, y + 0.5)`, in the image's absolute pixel coordinates, is
read at `(x + 0.5) * aspect / (scaleX * size)` and `(y + 0.5) / (scaleY * size)` in cells, where
`scale` is the evaluation's proxy scale and `aspect` the pixel aspect. So a grain has its size in
full-resolution square pixels, a proxy samples the same pattern, and a region of interest or another
data window changes no pixel's grain.

Each cell holds one grain per channel, hashed with Jarzynski and Olano's `pcg3d` from the cell and a
32-bit stream (a closing `v ^= v >> 16` makes every bit usable): one 32-bit word per channel, 11 bits
each for the grain's position in the cell and 10 for its amplitude, in (-1, 1). A point sums each
channel's grains within distance 1, which lie in its own cell and the eight around it:

```text
n(p) = sqrt(15 / pi) * sum over grains g of (1 - |p - g|^2)^2 * amplitude(g)
```

Hashed positions leave no lattice in the grain's strength or shape, and a uniform amplitude's variance
1/3 times the squared weight's integral pi/5 gives the noise a mean of 0 and a standard deviation of
1, at any size and in any channel. Each channel's grains lie in their own places, as a stock's dye
layers do, so the channels are independent in sign and in where they are strong. A grain is about
`size` pixels wide at half its height.

**Time.** The stream is a 64-bit mix of the seed and the frame. `Every Frame` takes the frame by the
same exact arithmetic as a Time node's frame output, at the operation's clock (so under a retimed layer
it is that layer's frame), and the plan marks the operation as varying with time; each frame's grain
is then unrelated to its neighbours', and returning to a frame draws it again. `Held` takes frame 0
and does not vary with time, and neither does anything downstream because of it.

**On the GPU.** One fixed resident program over the input, with every value in its data, so a new
frame is new data and a new cache identity on one compiled program, and so is a toggle of `monochrome`,
one 0 or 1 in the data that one branch in the program reads. The hash is integer arithmetic,
and the sum is additions and multiplications the program marks `precise`, each exact on every device;
the luma's division is the wrapper's correctly rounded one. A pixel whose response is 0 runs no noise.

**Sensor Noise (#565).** With `mode` Sensor Noise the node draws the noise of a small sensor: strong in
the shadows, soft colored blotches over it, and optionally a pattern that stays put. It reads
`intensity`, `size`, the three amounts, `monochrome`, `shadowWeighting`, `chromaBlotches`,
`blotchSize` and `fixedPattern`, and not `shadows`, `midtones` or `highlights`; Film reads none of the
four Sensor Noise values. With `L` as above, `w = 1 / (1 + shadowWeighting * L / 0.18)` (1 at black,
0.5 at mid gray when `shadowWeighting` is 1, and falling toward white; 0 makes the noise even),
`i = intensity * sqrt(15 / pi)`, and the noise `n` of one stream at the pixel:

```text
rgb' = rgb + a * w * i * k * n_grain             (n_grain per channel, or red's with monochrome)
          + a * w * i * chromaBlotches * (n_blotch - mean(n_blotch))
          + a * w * i * fixedPattern * n_fixed
```

The first term is the Film grain, here weighted by `w`. The second reads the same noise at a scale of
`size / blotchSize` (so a blotch is `blotchSize` pixels wide) from a stream of its own, and subtracts
the mean of its three channels, so the blotches shift color and not brightness. The third is the noise
at the grain's scale from a third stream. Each stream is the seed offset by a whole 2^32 (so it is past
every seed) mixed as above; the grain and the blotches take the frame (`Held` takes 0), and the fixed
pattern always takes frame 0, so it is the same on every frame and under `Every Frame`. A term whose
amount is 0 adds nothing and runs no noise.

*Cache identity.* The prepared table gains eight values after the geometry, and holds the two frame
streams and the fixed one. Film's table, identity, pixels and program text are as before the mode
existed. A frame that has only the fixed pattern (no grain amount and no blotches) names no frame, so
every frame has one identity and, on the GPU, one resident result. The plan then does not mark the
operation as varying with time (unless an amount is animated or driven), so the evaluator's operation
key leaves the frame out and one frame's result serves the rest, even with `Every Frame`; with any
grain or blotches each frame has its own. Film mode lowers only the values Film reads and Sensor Noise only its own (the others
keep their defaults with no identity), so moving a value the mode ignores neither invalidates a cache
entry nor changes a pixel.

*On the GPU.* The same program holds one more function and one line that enters it when the table's
mode slot is 2 or 3 (Sensor Noise, plus one with `monochrome`). Division is the wrapper's correctly
rounded one, additions and multiplications are `precise`, and the mean is taken with the binary32
nearest 1/3.

**Evidence.** `src/runtime/tests/film_grain_tests.cpp` pins the hash's values, holds the noise to its
mean, deviation and independence (in sign and in strength) at several spacings, the pixel to the formula over HDR and partial
alpha, and each promise above, frame and seed identity through the evaluator's memoization, the
domains, the fixed program, the pixel budget and cancellation. It also pins digests, taken from the
build before `monochrome` existed, of the pixels of every path, the program data, the cache identity
and the program with that one branch removed, and holds `monochrome` to its formula, its red channel
to the bits it has with the option off, a neutral pixel to neutral, and each state to its own cache
identity. For Sensor Noise it holds the formula to an independent reading, the shadow weighting
(darker is noisier, 0 is even), blotches that add no brightness, are soft and widen with their size,
a fixed pattern the same on every frame and animation while the grain varies, seeds, the cache identity
of each case, and Film mode to ignoring every Sensor Noise value; the digests and program pin above
hold with the Sensor Noise function and its entry line removed. The native harness (`runFilmGrains`)
holds twenty-two cases (the default, every path at three sizes, held, a proxy and a region of interest,
each with `monochrome` off and on, and Sensor Noise with every term, `monochrome`, held, fine, coarse,
a proxy, a region of interest and a fixed pattern alone) to the CPU oracle
within 2e-6 on RGB with exact alpha, warm RAM reuse with zero dispatches, the next frame with new grain
(held: zero dispatches and the same pixels), a changed value re-running with parity, and a toggle of
`monochrome` re-running with parity while going back finds its own entry with zero dispatches.
`tests/integration/film_grain_monochrome_migration_tests.cpp` opens a node saved without the option to
the same plan and bit-identical pixels, and `tests/integration/film_grain_sensor_noise_tests.cpp` does
the same for a node saved without the mode and its values, and round-trips a Sensor Noise node. `effect.FilmGrainKernel` and `node.bloom.film-grain` prepare through the production builder at
full frame and ROI. [Benchmarks](../development/benchmarks.md#film-grain-486) holds its cost.

## Codec Degrade

`bloom.codec-degrade`, schema 1, under Stylize (issue #563), has one Image in, `input`, one Image out,
`image`, and the optional effect mask every filter takes. It imitates the look of block-coded video;
no encoder runs. `quality` and `persistence` have sockets and can be keyed or driven; the other three
are constants:

| Role | Default | Domain | Meaning |
|---|---|---|---|
| `quality` | 40 | 0 to 100 | The JPEG quality scale of the quantiser |
| `persistence` | 0.5 | 0 to 1 | The share of the previous picture a predicted block keeps |
| `blockSize` | 8 px | 8 px (0), 16 px (1) | The edge of a coded block. Constant |
| `gopLength` | 12 frames | 1, 2, 4, 6, 8, 12, 15, 24, 30 frames (0 to 8) | Frames from one keyframe to the next. Constant |
| `chromaBleed` | on | off, on | Colour at half resolution in each direction (4:2:0 chroma subsampling). Constant |

**One coded picture.** The colour is first put on a display-referred curve, `e = (1 + k) v / (v + k)`
with `k = 0.25`, per channel on the straight (alpha divided out) colour, so HDR values keep their
range and the quantiser sees a perceptual scale; the inverse runs at the end. It is then converted to
Rec. 709 Y, Cb and Cr. Each plane is cut into `N` by `N` blocks (`N` is 8 or 16) on a grid anchored at
the image's absolute pixel origin, so a region of interest or another data window changes no block.
Every block takes an orthonormal type-II DCT, each coefficient is divided by the base step times a
frequency weight and rounded, and the block is reconstructed with the inverse DCT. The base step is
`0.0004 * s * N / 8` with the JPEG scale `s = 5000 / q` below 50 and `200 - 2q` from 50 up (`q` at least
1, `s` at least 1), and the weight grows from 1 at the DC coefficient to 8 at the highest frequency,
linearly in `u + v`. With `chromaBleed` on, Cb and Cr are first averaged over 2 by 2 pixels, coded at
that resolution (so a chroma block spans `2N` pixels) and spread back out. Alpha is not touched, and an
alpha of 0 stays transparent black.

**Predicted pictures.** After a keyframe each picture is coded against the one before it. For each
luma block the optical-flow vector is averaged over the block, rounded to a whole pixel, and the previous
picture is read at that offset (clamped at the frame edge) as the prediction. The residual of the
current picture against the prediction is quantised the same way, with these changes:

```text
step     = base step * weight * (1 + 3 * min(max(|mx|, |my|) / N, 1))
residual rounding offset = 0.5 - 0.4 * persistence
residual kept            = 1 - persistence
```

So a block that moves is quantised up to four times as coarsely, and persistence widens the dead zone
and scales the residual that gets through: at 1 nothing new is added and the block is the moved previous
picture, and at 0 the residual is quantised as in a keyframe. A block with no previous picture is
predicted from mid grey.

**Time.** The frame `f` is the operation's clock frame, taken as a Time node's frame is, so a retimed
layer counts its own. Its keyframe is `floor(f / gop) * gop`, and its position `p = f mod gop` (a
negative frame floors the same way). A frame renders the pictures of the keyframe through `f`, at most
`gop` of them, never reading a previous render: step 0 is the keyframe's Encoded source, step `i` for
`1 <= i <= p` is predicted from step `i - 1` with the optical flow between source frames
`keyframe + i - 1` and `keyframe + i`, and steps beyond `p` are the exact identity. A frame is
therefore the same in preview, scrubbing, backward play and export. The plan lowers the chain as
fixed-length: an Encode pass, `gop` Step passes, the optical flow between each pair of consecutive source
frames (`kFlowMethodDis`, draft quality, extended borders), and a Finish pass. Each time-shifted source is a time
sample of the same source subgraph, clamped to the keyframe, so the steps beyond `p` fold into
the identity and their flow solves are shared with the active ones by the operation cache. The
operations are marked as varying with time, and a change to `quality`, `persistence`, the block size,
the GOP length or `chromaBleed` is a new plan or new data and a new cache identity.

**On the GPU.** One fixed resident program per pass (`ColorGradeKind::CodecDegrade`) with every value
and the DCT basis in its table, so there is no per-block host work. An Encode pass converts to the Encoded
colour, so each Step reads it without repeating the curve. A predicted pass reads the chain so far and
the flow beside the Encoded colour in an atlas, as Z Defocus does, and a Finish pass inverts the curve and
the colour matrix. The arithmetic is additions, multiplications, floor and correctly rounded
divisions in one fixed order, with the quantiser dividing by a precomputed inverse weight, so the CPU
reference and the GPU agree exactly on the same input. Under a proxy the block is `round(N * scale)`
(at least 2) and chroma stays at full resolution below a scale of 0.75; the flow is read at proxy
resolution.

**Limits.** The motion is the optical flow between source frames, so wrong flow (fast motion, repeating
patterns, a cut) moves blocks wrongly, and the flow solves differ slightly between the CPU and the GPU
on a real moving picture, which a quantiser can turn into a visible difference ([Motion blur,
"Parity"](motion-blur.md#parity-determinism-and-identity) describes the same amplification). The
test scenes feed identical inputs to both. The look-back is capped at the GOP length (30 frames at most), so a
frame never reads further back than its keyframe.

**Cost.** A frame runs a chain that is as long as its position in the GOP, so the average cost grows
with the GOP length and the worst frame costs about `gop` flow solves and coded passes. A 16 px block
runs a larger DCT per pixel on the GPU than an 8 px block. Neither has been measured yet:
[Benchmarks](../development/benchmarks.md#codec-degrade-563) lists the scenario that will, and no budget is set.

**Evidence.** `src/runtime/tests/codec_degrade_tests.cpp` holds the colour round trip, the keyframe's
quantiser and grid, chroma bleed, the predicted pass's persistence, motion and coarsening, absent
images, windows and proxies, the stages, the program table, the pixel budget, cancellation and row
bands. Over whole plans it holds a frame independent of how it is reached (a keyframe on its own, scrub
against play, backward, a still picture), persistence freezing blocks until the next keyframe and
refreshing at it, blocking worse in the direction of motion, and a changed parameter changing the
frame. The native harness (`runCodecDegrades`) holds a frame inside a GOP, a keyframe, 16 px blocks
without chroma bleed, proxies and a region of interest to the CPU oracle with exact alpha, warm RAM
reuse with zero dispatches, the next frame and a changed value. `effect.CodecDegradeKernel` and
`node.bloom.codec-degrade` prepare through the production builder at full frame and ROI.
`testCodecDegradeLowering` holds the lowering.

## Denoise & Smooth

Denoise & Smooth (`bloom.denoise`, schema 1, issue #514) is a Filters node: one Image in, one Image out,
and the effect mask every filter takes. Its roles are `mode` (Median, Bilateral, Spatial, Temporal;
default Bilateral), `radius` (full-resolution pixels, 0 to 6, default 2, animatable), `strength` (0 to 10
in linear scene values, default 0.1, animatable), `frames` (Temporal only, 2 to 16, default 5),
`autoStrength` (a constant switch, default off) and `sampleFrom` / `sampleTo` (two opposite corners of
the noise sample region in field units, pixels from the composition centre with +y up; constants,
default a 64 x 64 square at the centre). The radius and strength have sockets, so a value graph drives
them.

Which parameters a node reads is one rule, `document::isParameterInactive`
(`parameter_activity.hpp`): `frames` only in Temporal, `radius` in every mode but Temporal, `strength`
only while Auto strength is off, and the sample region only while it is on (the same rule hides Gaussian
Blur's vertical radius until Separate axes is on and its channel radii until Per-channel radius is on;
Vignette's Amount or Exposure by its mode, Film Grain's Sensor Noise values or its response values by
its mode, and each Colorize mode's own colours, ramp, repetitions, phase and weights).
Properties, the Timeline and the node card hide an inactive row through it (the card's sample region pad
shows only while the region is active), and the snapshot compiler compiles no animation curve for an inactive parameter, so a
key left on a value the node does not read is neither lowered nor refused by the evaluator as a curve
nothing references.

The four modes are one kernel, `DenoiseKernel` (`denoise_image_effect.hpp`), whose host table is
the whole interface to the CPU reference and to the one fixed GPU neighborhood program
(`bloom.render.denoise.v1`, six vec4 of data). The output has the input's data window. The spatial modes
read the square of radius (rx, ry) around each pixel, skipping texels outside the window, over the stored
(associated) channels; a Temporal step reads 3 x 3 patches of two frames (below).

- **Median** takes the middle of the sorted values present (the upper middle on an even count). A
  channel keeps its own value unless it differs from that median by more than `strength`, so strength 0 is
  the plain median and a larger strength removes only outliers (Dust & Scratches). Alpha is its own channel.
  Color is taken as straight (unassociated) values from the neighbors with alpha above 0 and then takes the
  output alpha, so at a soft alpha edge the color stays the subject's color and a premultiplied value never
  exceeds its alpha; a center that keeps its color and its alpha is returned bit for bit. Where the output
  alpha is 0 (light that covers nothing) the premultiplied values take the same rule on their own.
- **Bilateral** is `p + sum(w (q - p)) / sum(w)` over the neighbors q, with `w = ws(dx) ws(dy) wr`. The
  spatial weight is the compact biweight `(1 - (d / (r + 1))^2)^2`; the range weight is the same shape
  over the squared distance of the four channels to the center pixel's, divided by `strength^2`, so a
  neighbor further than `strength` away does not contribute and edges stay.
- **Spatial** is the same fold with the range weight taken from the mean squared difference of the 3 x 3
  patch around the center and around the candidate (patches clamp to the window's edge): a non-local
  means search. Texture that repeats inside the search radius is averaged even where single pixels differ.
- **Temporal** is temporal non-local means (Buades, Coll and Morel, "Denoising image sequences does not
  require motion estimation", 2005) over the look-back window of the input (`look_back_recipes.hpp`
  `appendTemporalDenoise`, the frames up to and including the current one on the plan's frame grid). It
  lowers to the window's Time Samples and one `DenoiseKernel` step (mode Temporal) per earlier frame,
  each step reading the running result, the current frame and that earlier frame (`fieldImages`). For a
  pixel, an earlier frame's weight is the compact biweight `(1 - d / s^2)^2` of `d`, the mean squared
  difference of the 3 x 3 patches around the pixel in the current and the earlier frame over the four
  stored channels, with `s` the strength; the current frame's weight is 1. The result is the weighted
  mean of the colour channels, folded one frame at a time: `m' = fma(w / W', e - m, m)` with
  `W' = W + w`, carrying `1 / W` in alpha between steps (the first step starts from the current frame
  with `W = 1`; the last step writes the current frame's alpha back). Where an object moved, the patches
  differ by more than the strength, the weight is 0 and that frame is ignored: this is the motion
  rejection, and it needs no motion estimation. Static noise averages down nearly as a plain mean does
  (on the 4-frame test plate, noise 0.0300 to 0.0142 against 0.0134 for the plain mean) and a moving
  edge does not ghost (0.023 against 0.308 for the plain mean). Alpha is the current frame's, so a
  matte never smears. A frame before the composition's first repeats its first, the same in preview,
  scrubbing and export. On the device a step is the same fixed program's Temporal branch over an atlas
  of the running result, the current and the earlier frame (each moved exactly into place, as Codec
  Degrade lays out its frames), with the CPU reference's table and cell geometry
  (`temporalDenoiseProgramData`).

**Auto strength (noise profile).** With `autoStrength` on, the strength is measured: the lowering
builds a statistics plan of its own (`lowerDenoiseSampleReduction`) that crops the node's input to the
sample region (field units to crop margins against the composition size, clamped to the frame) and
reduces it with the Image Statistics reduction over the green channel's stored values, and feeds its
`deviation` (population standard deviation) through a value sink into the strength as
`min(deviation * factor, 10)`. The factor is the mode's (`denoiseAutoStrengthFactor`): Median 3 (an
outlier is beyond three deviations), Bilateral `2 sqrt(6)` (its range distance sums four channels'
squared differences between two noisy pixels, eight variances), Spatial and Temporal `sqrt(6)` (their
patch distance is a mean over the four channels, two variances, so the factor keeps the weight of
two noisy copies of the same content above zero). The measure is the Image Statistics route
(`image-statistics.md`): prepared before the frame on the worker evaluating it, at the frame's own
time and resolution, so it follows the footage; cached by node, revision and time, with the plan
compared on reuse, so moving the region or changing the input re-measures; on a device route its only
readback is the two-pixel statistics result (`prep.statistics.result_readback`), never the picture.
The interactive viewer and RAM preview measure it on the application's device route before the
frame's GPU scene is prepared, so a frame with Auto strength on is prepared and shown on the GPU there
too (as any Image Statistics frame is); when that route cannot measure, the frame takes the CPU
reference path and is labelled a CPU frame. Final renders and the headless and scripted routes
measure on the device. Wiring
an Image Statistics node's `deviation` into `strength` by hand remains possible.

All weights are polynomials of the host's float inputs and sums are `fma` folds in scan order (dy, then
dx), so the device repeats the CPU's arithmetic and the mean about the center keeps equal neighbors exact.
The radius is authored in full-resolution pixels and scaled to the evaluation resolution
(`round(radius * scale)` per axis, at most 6); a radius that rounds to zero is the identity and aliases its
input. The cache identity is the prepared table (mode, both radii, threshold or inverse variance, and for a
Temporal step its first and last flags, beside the keys of the images it reads), so a render scale, a keyed
radius, a changed or measured strength or another frame of the window never reuses another result.

**Cost per pass.** Reads per output pixel: Median and Bilateral (2r+1)^2 texels in one dispatch (Median
also sorts them per channel, O(n^2) insertion on the device, n up to 169); Spatial 9 patch texel pairs for
each of the (2r+1)^2 candidates, so nine times Bilateral; Temporal `frames - 1` Time Sample reads and the
same number of one-dispatch steps, each reading 18 patch texels and two more per pixel; Auto strength one
statistics reduction of the sample region per frame (cached). The compute is the texel reads and their fused multiply-adds; nothing
waits on the host: the pass is one dispatch with its table in a storage buffer (a keyed radius or strength
changes data, never the shader), no readback and no upload per frame beyond the six vec4, and the window
is the input's, so no halo is allocated. The CPU reference is the same loops, checked for cancellation
every sixteen pixels and every row. `tools/bench` has the cases (`denoise-<mode>-<radius>`,
`denoise-temporal-5`, and the export scenario `denoise-temporal-auto-2k` for Temporal with Auto strength) and
the budgets in `NODE_COST_BUDGETS` are relaxed per mode from this model; they are marked unmeasured until a
run replaces them. Cancellation, budget and retirement follow the kernel: a Temporal step checks
cancellation as the spatial passes do and publishes nothing when cancelled, each step and each window frame
is charged against the frame's pixel budget like any pass, and the results retire with the operation and
scene caches; the Auto strength measure follows the Image Statistics rules (`image-statistics.md`).

**Limits.** The radius stops at 6 so the Median sort and the Spatial search stay bounded in registers.
Spatial's and Temporal's patch is fixed at 3 x 3. Temporal compares each pixel with the same pixel of the
earlier frames (no search window), so moving content is kept sharp by rejection rather than averaged
along its motion: a moving area keeps the noise of the current frame. The sample region is constant
(not animated); place it on a flat area that stays flat through the shot.

## Sharpen

`bloom.sharpen`, schema 1, under Blur & Sharpen (issue #486), has one Image in, `input`, one Image out, `image`,
and the optional effect mask every filter takes. Every value has a socket and can be keyed or driven:

| Role | Default | Domain | Meaning |
|---|---|---|---|
| `amount` | 1 | 0 to 10 | How much of the detail is added back |
| `radius` | 3 | 0 to 256 | The blur's radius in full-resolution pixels, as Gaussian Blur's: sigma radius / 3 |
| `threshold` | 0 | 0 to 1000 | Detail in linear light left alone, in each channel of the straight colour |

**The chain.** The node lowers to two operations, in `compositing_recipes.cpp`: the input's Gaussian blur
with the frame edge clamped (the ordinary separable blur, so its cache, budget and retirement are the
blur's), then `SharpenKernel` over the input and that blur.

**The pixel.** With the premultiplied input `rgb` and `a` and its blur `B` at the same pixel, in binary32:

```text
detail = rgb - (a / B.a) * B.rgb
cored  = detail - a * threshold    where detail >  a * threshold
       = detail + a * threshold    where detail < -a * threshold
       = 0                         otherwise
rgb'   = max(rgb + amount * cored, min(rgb, 0))
a'     = a
```

Comparing colour with each image's own alpha taken out means a flat colour stays that colour through an
alpha ramp or a hard alpha edge, where blurring premultiplied pixels would otherwise read as detail. The
sum is in proportion to alpha, so the image stays premultiplied, alpha 0 stays transparent black and no
division by the input's alpha happens. There is no clamp above: a highlight's overshoot keeps its range,
and with no threshold the operation is linear in the exposure. Below, an undershoot stops at black so
that a dark halo cannot put negative light into what follows; a channel already negative keeps its own
value as the floor. The threshold is soft, so a value of detail just past it changes the result by just
a little. A subnormal result is zero on every device. The node is the identity at amount 0 or radius 0,
which aliases the input.

**On the GPU.** One resident dispatch of `ImageCombineV1`'s Unsharp mode (mode 12) over the input and the
blur, the amount in its factor and the threshold in its push constants: no atlas and no readback. It
performs the operations above with `fma` and the correctly rounded division (`roundedDivide`, the same
definition as `ocioGpuPreciseDivisionGlsl`), so the difference of nearly equal values matches the CPU
reference's. An amount or threshold is a new key for this pass and what follows it: the blur stays
cached. A radius is a new blur too. Only a non-finite blur pixel sets the shader's status flag: a blur's faint
tails make subnormal pixels ordinary there, and they change nothing.

**Evidence.** `src/runtime/tests/sharpen_tests.cpp` holds the pixel to an independent double-precision
reading over HDR, signed and partial-alpha values, a flat colour flat through its alpha edges, a step
edge's halo against an independent Gaussian, the threshold, exposure invariance, the data window, a
region of interest equal to the whole frame's pixels, identity, the domains, the pixel budget and
cancellation. `gpu_image_combine_native_tests.cpp` holds the Unsharp mode to the CPU pixel over HDR,
signed and partial-alpha pixels and refuses a bad amount or threshold before dispatch. The native harness
(`runSharpens`) holds six cases (the default, strong, wide, a threshold, a half-resolution proxy and a
region of interest) to the CPU oracle within 2e-6 on RGB with exact alpha, warm RAM reuse with zero
dispatches, a changed amount or threshold keeping the blur, and a changed radius running the blur's two
passes again, each with parity.
`effect.SharpenKernel` and `node.bloom.sharpen` prepare through the production builder at full frame and
ROI. [Benchmarks](../development/benchmarks.md#sharpen-486) holds its cost.

## Image Math, Map Range and Clamp

`bloom.image-math`, `bloom.image-map-range` and `bloom.image-clamp`, schema 1, under Channels (issue
#481), do arithmetic on pixels so render passes (mist, Z-depth, normals, motion vectors, IDs) can be
normalized and limited as a single value can. Each has one Image in, `input`, one Image out, `image`,
and the optional effect mask; Image Math also has an optional Image `second`. They lower to one
`PixelMathKernel` image effect (a grade kind) with no new operation boundary.

| Node | Role | Default | Domain |
|---|---|---|---|
| Image Math | `operation` | Multiply | Add, Subtract, Multiply, Divide, Power, Minimum, Maximum, Absolute |
| | `value` | 1 | -1,000,000 to 1,000,000 (the operand when `second` is not connected) |
| Image Map Range | `inputMin`, `inputMax` | 0, 1 | -1,000,000 to 1,000,000 |
| | `outputMin`, `outputMax` | 0, 1 | -1,000,000 to 1,000,000, in either order |
| | `clamp` | off | limits the result to the output range |
| Image Clamp | `minRed` ... `maxBlue` | 0, 1 | -1,000,000 to 1,000,000 |
| | `minAlpha`, `maxAlpha` | 0, 1 | 0 to 1 |
| all | `channels` | Color (RGB) | Color (RGB), Red, Green, Blue, Alpha, Color and alpha (Clamp always limits all four) |
| all | `space` | Straight color | Straight color, Stored values |

Every scalar has a socket and can be keyed or driven; the choices and `clamp` are constants.

**Premultiplication.** With Straight color the stored red, green and blue are divided by alpha (0 where
alpha is 0), changed, and multiplied by the result's alpha, so a soft edge keeps its color and a changed
alpha re-weights the color. With Stored values the premultiplied values are changed as they are, which is
what a data pass wants. A channel that is not selected keeps its stored value exactly. Alpha is always
limited to [0, 1]. Alpha passes through unless Alpha or Color and alpha is selected.

**The pixel**, in binary32 and in this order. Non-finite input is read as 0. Add, Subtract and Multiply
are the plain operations; Divide by 0 is 0; Minimum and Maximum are `min` and `max`; Absolute ignores the
operand. Power is `0^0 = 1`, 0 to any other power 0, a negative base needs an integral exponent (an odd
one keeps the sign) and is otherwise 0. A result that is not finite is 0. Map Range is
`fma(x - inputMin, scale, outputMin)` with `scale = (outputMax - outputMin) / (inputMax - inputMin)`
limited to 1e30 in size, an empty input range mapping everything to `outputMin`, and `clamp` limiting to
the ordered output range. Clamp is `min(max(x, lo), hi)` per channel, a minimum above the maximum being
the maximum. The second image's pixel is read at the same position, as straight color in Straight mode,
and is transparent black outside its data window or when it has no pixels.

**Identity and cache.** The pass is never an identity alias (it also replaces non-finite values), so
there is no storage alias to a neighbor. The packed table (36 floats, 40 with a second image) is the
cache identity; changing any value, the operation, channels or space changes it.

**On the GPU.** One fixed program (`bloom.render.pixel-math.v1`) of the generic color grade dispatch
reads the same table. With a second image, the input and the second image's window are placed in an
atlas twice the output window's width (the second moved by one width and blended Normal into the empty
half), one dispatch computes inside the output window, and the output window is cut back out. Power's
logarithm and exponential are each backend's own, so the two agree to within their last bits (2e-6
relative on RGB, alpha exact).

**Evidence.** `src/runtime/tests/pixel_math_tests.cpp` reads real values for every operation, the
failure rules, channels, both premultiplication modes, Map Range and Clamp, a second image over
differing windows, the domains, the table's identity, the pixel budget and cancellation.
`snapshot_compiler_effect_tests.ipp` holds the lowering; the native harness (`runPixelMaths`) holds every
operation, mode, channel choice and second-image window to the CPU oracle with cold, warm and changed-
value runs; `effect.PixelMathKernel` and the three `node.*` fixtures prepare through the production
builder in the coverage contract. [Benchmarks](../development/benchmarks.md) lists `image-math-2k`.

## Tone Map

`bloom.tone-map`, schema 1, under Color Correction (issue #518), compresses highlights for HDR
scene-linear images with a soft clip, recovers shadows and, when asked, pulls colors that left the gamut
back inside. It has one Image in, `input`, one Image out, `image`, and the optional effect mask. It
lowers to one `ColorToolKernel` image effect (a grade kind, `ColorTool`) with no new operation boundary.

| Role | Default | Domain |
|---|---|---|
| `method` | Each channel | Each channel, Brightest channel |
| `knee` | 0.8 | 0 to 1,000,000 |
| `ceiling` | 1 | 0 to 1,000,000 |
| `shadows` | 0 | -1 to 16 |
| `shadowRange` | 0.18 | 0.0001 to 1,000,000 |
| `gamutCompress` | off | on, off |
| `gamutThreshold` | 0.8 | 0 to 0.999 |
| `gamutLimit` | 1.2 | 1.001 to 100 |

Every scalar has a socket and can be keyed or driven; `method` and `gamutCompress` are constants. On
the card, `knee` and `ceiling` are one range control (Roll-off range).

**The pixel**, in binary32 and in this order, on the straight color (the stored color divided by alpha);
the result is multiplied by alpha again and alpha is untouched. A pixel with alpha 0 is left as stored,
since it has no straight color. A pixel whose color is not finite is an error, like every grade.

1. *Gamut compression* (when `gamutCompress` is on). With `a = max(r, g, b)` and `m = |a|`, each
   channel `c` is at the distance `d = (a - c) / m` from the brightest channel (0 when `m` is 0, the
   pixel then being left alone). A distance up to `gamutThreshold` (`t`) is left exactly as it is.
   Above it, with `e = d - t`, the distance becomes `t + e / (1 + e / s)` and the channel
   `a - m * (that)`, where `s = (l - t)(1 - t) / (l - 1)` and `l` is `gamutLimit`. This is the rational
   (power one) form of the common reference gamut compression curve: a distance of exactly `l` lands
   on the edge of the gamut (the channel becomes 0), larger distances are pulled in without limit
   toward `t + s`, and the curve never decreases, so a farther color never ends nearer the edge.
2. *Shadow recovery* (when `shadows` is not 0). Each channel `c` is multiplied by
   `1 + shadows * R / (R + max(c, 0))` where `R` is `shadowRange`. The gain is `1 + shadows` at black,
   `1 + shadows / 2` at `R` and falls toward 1 above it, so black stays black, a shadows of -1 takes the
   darkest values toward black and a larger value lifts them by as much as `1 + shadows`. A negative
   channel takes the gain at 0.
3. *Highlight roll-off.* With `room = ceiling - knee`: a value at or below `knee` is untouched;
   above it, with `d = x - knee`, the value is `knee + room * d / (d + room)`, which leaves the knee
   with slope one (no corner), never decreases and approaches `ceiling` without reaching it. A `room`
   at or below 0 is a hard clip, `min(x, ceiling)`. *Each channel* rolls every channel off on its own;
   *Brightest channel* rolls off `max(r, g, b)` alone and multiplies all three channels by the
   factor the brightest one changed by, so hue and saturation hold. A negative channel is never
   touched by the roll-off.

The default (knee 0.8, ceiling 1) maps a scene-linear 1.0 to 0.9 and 4.0 to about 0.988; to leave the SDR
range alone and only tame an HDR peak, raise both, for example a knee of 4 and a ceiling of 8.

**Identity and cache.** The pass is never an identity alias. The packed table (32 floats: the
operation, the method and gamut switch, the knee, ceiling and room, the shadow gain and range, the
gamut threshold and scale) is the cache identity; changing any value or choice changes it. Division is
the correctly rounded one on both sides and nothing else is transcendental, so the CPU reference and
the GPU program agree exactly.

**On the GPU.** One fixed program (`bloom.render.color-tool.v1`) of the generic color grade dispatch
reads the same table, with `precise` operations in the CPU's order. A non-finite result is refused as
on the CPU. It runs resident with the image and needs no readback.

**Evidence.** `src/runtime/tests/color_tool_tests.cpp` reads real values for the roll-off, the hard clip,
both methods, shadow recovery and gamut compression, the properties above over a sweep, premultiplied
and alpha 0 pixels, the domains, the table's identity, the pixel budget and cancellation.
`snapshot_compiler_effect_tests.ipp` holds the lowering; the native harness holds four variants to
the CPU oracle with cold, warm and changed-value runs; `effect.ColorToolKernel` and `node.bloom.tone-map`
prepare through the production builder in the coverage contract. [Benchmarks](../development/benchmarks.md)
lists `tone-map-2k`, written and not yet run.

## Color Matrix

`bloom.color-matrix`, schema 1, under Color Correction (issue #517), mixes the channels of an image
with a 3 x 3 or 4 x 4 matrix and converts between color models. It has one Image in, `input`, one
Image out, `image`, and the optional effect mask, and lowers to the same `ColorToolKernel` image
effect (grade kind `ColorTool`) as Tone Map.

| Role | Default | Domain |
|---|---|---|
| `preset` | Custom matrix | Custom matrix, Identity, Gray (luminance), Swap red and blue, Swap red and green, Swap green and blue, Sepia |
| `matrixSize` | 3 x 3 | 3 x 3 (color), 4 x 4 (color and alpha) |
| `inputModel` | RGB | RGB, HSL, HSV, YCbCr, Lab |
| `outputModel` | RGB | RGB, HSL, HSV, YCbCr, Lab |
| `rr` ... `aa` | the identity | -100 to 100; `xy` is the share of input `y` in result `x` |

The sixteen elements have sockets and can be keyed or driven; the choices are constants. A preset
other than Custom replaces the elements with its own matrix (the elements are then not read); the
alpha row and column of a preset are the identity's.

**The pixel**, in binary32, on the straight color (stored color divided by alpha). A pixel with alpha
0 is left as stored, so a matrix cannot make alpha from nothing.

1. The input is read as `inputModel` and converted to RGB.
2. Each result channel is `((m0 * r + m1 * g) + m2 * b)`, and at 4 x 4 also `+ m3 * a`, for the red,
   green and blue rows; at 4 x 4 the alpha row is the same sum, limited to [0, 1]. At 3 x 3 alpha is
   unchanged and the alpha column is not read.
3. The result is converted from RGB to `outputModel`, multiplied by the result's alpha, and a result
   alpha of 0 is transparent black.

The models are on scene-linear values, with the hue a fraction of a turn. HSV is
`(h, max - min over max, max)`, defined for HDR values, negatives limited to 0. HSL is the standard
HSL and limits RGB and, going back, saturation and lightness, to [0, 1]: use HSV, YCbCr or Lab for
values above 1. YCbCr uses the Rec. 709 luma weights on linear values (`Cb = (B - Y) / 1.8556`,
`Cr = (R - Y) / 1.5748`, zero for gray). Lab is CIE Lab for linear Rec. 709 primaries and D65 white,
with L, a and b divided by 100 so white is 1 and the other axes lie within about -1.3 to 1.3; its cube
root is four Newton steps from a bit-level first guess, the same exact operations on both routes,
and values below the linear segment of Lab's curve (including negatives) use its linear part. A
non-RGB result is stored premultiplied like any color, so convert back before compositing a
partly covered image.

**Identity and cache.** An identity matrix between RGB and RGB aliases its input exactly. Otherwise the
packed table (32 floats: the operation, size and two models, then the sixteen elements after any
preset) is the cache identity. Every operation is exact in binary32 with correctly rounded division, so
the CPU reference and the GPU program (the same fixed `bloom.render.color-tool.v1`) agree bit for bit.

**Evidence.** `color_tool_tests.cpp` reads preset, custom, 3 x 3 and 4 x 4 results, known values of each
model (red, white, gray, HDR) and the round trip of every model, the domains and the identity.
`effect.ColorToolKernel` and `node.bloom.color-matrix` prepare through the production builder, and
the native harness holds ten variants (each model out and back, a 4 x 4 mix and a preset) to the CPU
oracle. [Benchmarks](../development/benchmarks.md) lists `color-matrix-2k`, written and not yet run.

## Quantize

One node, `bloom.quantize`, under **Stylize** (#519): a **Threshold** that turns the picture into
black and white (on luma or on each channel, hard or soft) and a **Posterize** that snaps each channel
to a number of levels. It is a color tool kernel like Tone Map and Color Matrix
(`ColorToolKernel`, operation Quantize): one pointwise pass over the straight color, the CPU reference
(`quantizePixel`) and the GPU program (`bct_quantize`) running the same binary32 operations over one
table, with no new GPU coverage exception. Values are in the working space (scene-linear by default),
so a threshold level of 0.5 is half the scene-linear value, not half of the display's brightness.

**Straight color and alpha.** Every operation reads the straight color (premultiplied divided by
alpha) and writes it back premultiplied. A pixel of alpha 0 stays transparent black. Threshold keeps
alpha. Posterize keeps alpha unless **Posterize Alpha** is on.

| Mode | Rule |
| --- | --- |
| Threshold | `v` is the straight value: Rec. 709 luma (`0.2126 r + 0.7152 g + 0.0722 b`, the same value for the three channels) with **Threshold on Luma**, each channel on its own with **Per channel**. A hard step (Softness 0) is 1 for `v >= level` and 0 below it, so a value exactly at the level is white. With Softness `s > 0` the result is `smoothstep` over `[level - s/2, level + s/2]`: 0 at and below the low edge, exactly one half at the level, 1 at and above the high edge. **Invert** replaces the result `t` by `1 - t`. |
| Posterize | A channel is clamped to `[0, 1]` and snapped to the nearest of `levels` equally spaced values: `round(x (levels - 1)) / (levels - 1)`, a half rounding up. The product and the fraction above its floor are taken in binary32 (the fraction is exact, so the half-way test is). With **Link levels** on, red, green and blue read **Levels**; off, they read **Red levels**, **Green levels** and **Blue levels**. **Posterize Alpha** snaps alpha the same way to **Alpha levels**; an alpha that snaps to 0 gives transparent black, and the color is scaled by the new alpha. |

**HDR and negative values.** The output of a threshold is 0 or 1 (or the ramp between them), so an HDR
value above the level is white and a negative one black. A posterize clamps its input first: a value
above 1 is the top level and a negative one the bottom level (the nearest end level), never an
out-of-range value.

**Domain.** Level and Softness are finite and in `[0, 16]`; levels are in `[2, 256]` (a real number: at
an integer it is exactly that many levels, and between integers the steps are spaced `1 / (levels - 1)`
apart, so a keyed value changes smoothly). A value the mode does not read is neither shown, nor
compiled (`isParameterInactive`, the lowering fixes it to a constant): a keyed value of the other
mode, or of the channels while Link levels is on, neither varies the picture with time nor invalidates
its cache. Mode, basis and the switches are constants with no socket; every scalar has one and can be
keyed or driven.

**Interface.** The card and Properties show Threshold as a span on a black-to-white luma strip (a
`KRangeSelector` with the luma strip on): its centre is the Level and its width the Softness, so one
handle drag writes both, as one undoable command, and a hard step is a span of no width. Posterize shows
the master Levels with a Link levels switch; unlinking shows Red, Green and Blue levels in place of the
master. Mode and Threshold on are choices; there is no color to pick.

**Evaluation.** One resident pass on the GPU route for the preview, the RAM preview, every export and
the headless render; a reduced render scale gives the same pixels (a pointwise pass reads nothing
around the pixel), and a region of interest reads only its own pixels. A muted node passes its input
through. The table is cached by value, so a changed level re-runs this pass only.

Cost, from the code: one read and one write per pixel with a divide per channel, about a dozen
arithmetic operations; nothing is read around the pixel. No timing is recorded; see
[Benchmarks](../development/benchmarks.md).

Tests: `quantize_tests` (known answers at the level and one ulp either side, every half-way step of
levels 2 to 129, levels 2 and 256, a sweep against independent binary64 rules, alpha, HDR, negatives,
domain refusal, cache identity of the table, the pixel budget and cancellation),
`snapshot_compiler_effect_tests` (lowering, inactive values, time dependence, mute), the Required
`node.bloom.quantize` GPU coverage fixture and the native CPU/GPU parity cases (boundary colours,
warm zero-dispatch, a changed value), the node-viewer evidence scenarios `quantize-*` (viewer, cold
then warm RAM preview), `properties_registry_tests` and `grading_cards_tests`.

## Dither

One node, `bloom.dither`, under **Stylize** (#527): it adds a small, tiled threshold pattern to the
color so that a later quantization to a lower bit depth (an 8-bit export, a 10-bit display) breaks
smooth gradients into fine noise instead of visible bands. The node quantizes nothing itself; it is
the noise that a quantizer after it needs. It is a color grade kernel (`DitherKernel`, semantics
`bloom.render.dither.v1`): one pointwise pass whose CPU reference (`ditherPixel`) and GPU program
(`bloom_dither`) compile the same kernel text, `src/runtime/dither_kernel.glsl`, over one table, with
no GPU coverage exception.

**Patterns.** Both are tables of ranks `0 .. n² - 1` over an `n × n` tile, read as a centred
threshold `t = (rank + 0.5) / n² - 0.5` in `(-1/2, 1/2)`:

| Pattern | Table |
| --- | --- |
| Ordered (Bayer) | Bayer's recursive matrix of side 2, 4, 8 or 16 (**Matrix size**): the rank of `(x, y)` sums `(2 (x_b xor y_b) + y_b) · 4^(k-1-b)` over the bits `b` of `x` and `y`. A regular cross-hatch, cheap and stable, that shows as a texture at large amplitudes. |
| Blue noise | A 64 × 64 void-and-cluster table (Ulichney 1993), generated in `core::dither::voidAndCluster` and never stored as an image: integer Gaussian energies (σ = 1.5, weights `round(2^30 e^(-d²/2σ²))` on the torus, the exponential from a fixed series so every platform builds the same integers), an initial 10% of cells placed by SplitMix64 from seed 1, the tightest cluster and largest void swapped until stable, then ranks given by removing clusters and filling voids, ties to the lowest index. The table is pinned by its SHA-256 (`dither_pattern_tests`); its power below 8 cycles is about 1.6e-5 of the total, against about 5% for white noise. |

**Seed, frame and channel.** Each channel reads the table shifted by `(sx, sy)` and with its ranks
rotated by `r` (all mod the tile), where `(sx, sy, r)` comes from a SplitMix64 mix of the **Seed**,
the composition frame and the channel. With **Pattern changes Every Frame** a new frame is a new
placement (temporal dither, which averages out over time); **Held** keeps frame 0's on every frame.
**Monochrome** gives the three channels red's placement, so the dither has no colour. A shift and a
rotation keep the table's spectrum, so every placement is equally blue (or equally Bayer).

**Amplitude.** **Amplitude** is the peak-to-peak size of the threshold in least significant bits
(LSB) of the **Target bit depth** (8, 10, 12 or 16 bits): the added value is
`t · amplitude / (2^bits - 1)`, so the default of 1 LSB moves a value by at most half a code value
either way, the classic dither for rounding to that depth. It is in `[0, 16]` (soft range 0 to 4),
animatable, and 0 is the identity, bit for bit.

**Domain.** The amplitude is measured either way:

| Measured in | Rule |
| --- | --- |
| Display (sRGB), the default | The straight value `s = v / a` is encoded with the sRGB transfer (IEC 61966-2-1, odd about zero so negatives keep their sign), the amount is added, the sum is decoded and premultiplied again. An LSB is then a code value of an sRGB-encoded file, which is how 8-bit deliverables are quantized. A straight value beyond ±65504 passes unchanged. |
| Stored value | The amount times alpha is added to the stored (working-space, premultiplied) value: for a quantization of the working values themselves (a linear or log file). |

The transfer's power and logarithm are the field kernel's polynomials (`bdk_exp2`, `bdk_log2`), every
intermediate a `precise` binary32 operation, so the CPU and each device give the same bits
(`docs/architecture/fields.md`).

**Alpha and HDR.** Alpha is never changed; a pixel of alpha 0 is left as it is. Values above 1 and
below 0 are dithered like any other (the node does not clamp); in the Display domain the decode of a
value near zero keeps its sign.

**Geometry.** The pattern is anchored to the full-resolution pixel grid: a pixel at data-window index
`(c, r)` reads the threshold of the full-resolution pixel `floor((c + origin + 0.5) / scale)`. A
region of interest reads the same thresholds as the full frame over its pixels, and a half- or
quarter-resolution proxy reads every second or fourth full-resolution threshold (so its pattern is
the full frame's, subsampled, not a new one).

**Interface.** The card and Properties draw a preview: a ramp cut to 4 bits without the dither (top)
and with the node's pattern, matrix, seed, monochrome and amplitude (bottom); the amplitude as a
grading slider row in LSB with a readout of what it is ("1.00 LSB of 8-bit = 0.00392 of full scale,
display encoded"). Pattern and Measured in are labelled radios, Matrix size shows for Ordered only
(`isParameterInactive`), Target bit depth and Pattern
changes are choices, and the seed's row has **Randomize seed**. Choices, seed and Monochrome are
constants with no socket; the amplitude has one.

**Evaluation.** One resident pass on the GPU route for the preview, the RAM preview, every export and
the headless render. The table (its header and ranks, at most 20 + 4096 floats) is built per frame
from the seed, frame and amplitude and cached by value; a held, unanimated Dither is time-invariant,
an Every Frame one depends on the frame. A muted node passes its input through.

Cost, from the code: one read and one write per pixel; per channel a table lookup and, in the
Display domain, a divide, two polynomial transfers and a multiply (about a hundred arithmetic
operations); nothing is read around the pixel. No timing is recorded; see
[Benchmarks](../development/benchmarks.md).

Tests: `dither_pattern_tests` (Bayer against the bit formula, the weights against the library
exponential, void-and-cluster against an independent brute-force implementation at 4, 8 and 16,
the pinned 64 × 64 digest, the spectrum, the placements), `dither_tests` (the reference against
binary64 rules, the identity, domain refusal, seeds and frames, region of interest and proxy rule,
cache identity, budget and cancellation, and the banding proof: a quantized gradient's mean run
length and low-frequency error with and without each pattern), `snapshot_compiler_effect_tests`
(lowering, cache identity), the Required `effect.DitherKernel` and `node.bloom.dither` GPU coverage
fixtures and the native CPU/GPU parity cases (every pattern and size, bit depth, domain, Monochrome,
Held, proxy, region of interest, warm zero-dispatch, a changed amplitude, the lifecycle), the
node-viewer evidence scenarios `dither-*` (viewer, cold then warm RAM preview),
`properties_registry_tests` and `grading_cards_tests`.

## HSL Qualifier

`bloom.hsl-qualifier`, schema 1, under Keying & Mattes (issue #523), selects the pixels of an image
whose hue, saturation and luma lie in chosen ranges, as a soft Mask. It has one Image in, `input`,
and one Mask out, `mask`, and like Luma Key (the other Image to Mask node) takes no effect mask. It
lowers to the same `ColorToolKernel` image effect (grade kind `ColorTool`) as Tone Map and Color
Matrix, with the operation `HslQualifier`; the Mask is carried as coverage in all four channels.

| Role | Default | Domain |
|---|---|---|
| `hueCenter`, `hueWidth`, `hueSoftness` | 0, 360, 20 degrees | 0 to 360, 0 to 360, 0 to 180 |
| `satLow`, `satHigh`, `satSoftness` | 0, 1, 0.1 | 0 to 1 |
| `lumaLow`, `lumaHigh`, `lumaSoftness` | 0, 1, 0.1 | 0 to 10000 (luma above 1 is HDR) |
| `invert` | off | constant |

The default selects every color. Every value can be keyed or driven through its socket.

**The pixel**, in binary32, on the straight color (stored color divided by alpha). A pixel with alpha
0 has no coverage.

1. Hue and saturation are HSV's (as in Color Matrix); luma is the Rec. 709 weighting
   `((0.2126 r + 0.7152 g) + 0.0722 b)`. A pixel with no saturation has hue 0.
2. Each range gives a selection. For hue, the distance is the shorter way round the turn from
   `hueCenter` minus half the `hueWidth`; for saturation and luma it is how far the value lies below
   the low end or above the high end. A distance of 0 or less is selected (1); past it the selection
   falls linearly to 0 over the softness; with no softness it is 0 at once.
3. The three selections multiply. `invert` makes it `1 - selection`. The coverage is the selection
   times the pixel's alpha.

**Identity and cache.** The qualifier is never an identity. The packed table (32 floats) is the cache
identity, and every operation is exact in binary32 with correctly rounded division, so the CPU
reference and the GPU program (the fixed `bloom.render.color-tool.v1`) agree bit for bit. A muted
HSL Qualifier is an empty Mask, never its Image.

**Not duplicated.** The hue curves of Curves (Hue vs Hue, Hue vs Sat, Hue vs Luma, ...) already change
a color by its hue; the qualifier only selects, and a Mask from it limits any other node.

**Evidence.** `color_tool_tests.cpp` reads known windows (red, green, wrap past 360 degrees, soft and
hard edges, gray, HDR white, invert, alpha) and the domain. `node.bloom.hsl-qualifier` prepares
through the production builder, and the native harness holds three variants to the CPU oracle.
`testHslQualifierChainPixels` compiles a checker through HSL Qualifier into Image Mix's `mask` and holds
every pixel of the mixed picture to the first image plus `factor * coverage` of the second, the coverage
computed per pixel in double precision from the documented model (a selected, a half-selected and an
unselected cell, an inverted selection and a factor of 0.6); a muted qualifier mixes nothing.
[Benchmarks](../development/benchmarks.md) lists `hsl-qualifier-2k`, written and not yet run.

## Edge Extend

`bloom.edge-extend`, schema 1 (#516), is a compositing recipe like Light Wrap, listed under
Compositing in the Add menu. It has a required Image input, `input`, and an Image output, `image`,
takes no effect mask, and has no pixel kernel of its own: the compiler lowers it to a chain of
existing primitives (`appendEdgeExtend`), so each keeps its own CPU reference, resident GPU command,
cache identity, ROI, budget, cancellation and retirement. Parameters: `distance` 4 pixels in
`[0, 256]` (a blur radius in full-resolution pixels, so it scales with the render scale) and
`amount` 1 in `[0, 1]`, both animatable.

A soft edge of a keyed or antialiased element stores a straight color polluted by what was behind
it, which shows as a dark or bright fringe once the element is placed on another background. Edge
Extend replaces the color under the soft part of the alpha with the color of the solid pixels near
it and never changes alpha. With premultiplied `p = a c` and alpha `a`:

1. Multiply Alpha scales the input by its own alpha: `a p` (so the blur below weighs each pixel by
   `a^2` times its straight color).
2. A Gaussian Blur by `distance` (transparent edge, RGBA) gives `B`.
3. Unpremultiply divides it by its own alpha (above `2^-20`): `c_x`, the straight color of the nearby
   pixels weighted by alpha squared, so solid pixels count for much more than the soft ones.
4. A Channel Combine puts the input's alpha back and premultiplies: `x = a c_x`, alpha `a`.
5. A white image held out by the input's inverted alpha (Channel Select of ones, Multiply Alpha with
   the invert switch) is the Mask `1 - a`.
6. Image Mix of the input with `x` by `amount`, through that Mask: `p' = p + amount (1 - a) (x - p)`.

A solid pixel (`a = 1`) is exactly the input, the alpha of every pixel is unchanged, amount 0 and
distance 0 are the input, and a region of one straight color is a fixed point. Because stored color
is premultiplied, a pixel of alpha 0 cannot hold color: Edge Extend changes the color of partly
transparent pixels only, which is what removes the fringe under later blurs, scales and
resamplings. The data window is the input's. The wide blur is the cost: two separable passes whose
support grows with `distance`; the other five operations are single pixel passes. A muted node passes
its input through.

**Evidence.** The direct recipe test compares the chain with an independent double-precision
reference (a direct two-dimensional Gaussian, the alpha weighting, the division and the mix) at
1e-4, and checks the alpha, the solid pixels, amount 0, distance 0 and a uniform color, at full and
at half render scale. At half scale the distance is half as many proxy pixels: the chain is held to the
reference run over the proxy's own input with half the distance, and the reference with the unscaled
distance must not match. Edge Extend's own cancellation (a pre-cancelled token, and a cancel arriving inside
the chain) returns no frame, and a pixel budget under the output, or under the chain's working set, is
refused with the pixel storage budget diagnostic and publishes nothing. The compiler test reads the seven operations and the parameter identities
they use. `node.bloom.edge-extend` prepares through the production builder (full frame and ROI),
and the native harness holds two variants at full scale and the same two at half render scale to the CPU
oracle with warm reuse and a changed amount.
[Benchmarks](../development/benchmarks.md) lists `edge-extend-2k`, written and not yet run.

## Alpha Contour

`bloom.alpha-contour`, schema 1 (#509), is a compositing recipe listed under Stylize in the Add menu: a
stroke of an image's alpha edge, drawn in image space. It has a required Image input, `input`, and an
Image output, `image`, takes no effect mask, and has no pixel kernel of its own: the compiler lowers
it (`appendAlphaContour`) to the exact mask distance transform (`MaskDistanceKernel`, the one Shape
Distance from a Mask runs) and a field raster over it (`FieldRasterKernel`), then a Normal Merge, so
each keeps its own CPU reference, resident GPU command, cache identity, ROI, budget, cancellation and
retirement.

| Role | Default | Domain |
|---|---|---|
| `width` | 4 | 0 to 256 pixels; animatable and drivable |
| `softness` | 0 | 0 to 64 pixels; animatable and drivable |
| `offset` | 0 | -256 to 256 pixels (positive outward); animatable and drivable |
| `color` | white | straight RGBA, HDR allowed; its alpha scales the band; animatable and drivable |
| `composite` | Over the image | Over the image, Under the image, Contour only (a constant) |

The input's coverage is its alpha at one half, the edge the distance transform reads. With `d` a
pixel's signed distance from that edge in full-resolution pixels (outside positive; half a pixel from
each pixel centre to the boundary between them), `h = max(width / 2, 1/2)`, `lo = h - 1/2 -
softness / 2` and `hi = h + 1/2 + softness / 2`, the band's coverage is
`k = clamp(1 - (|d - offset| - lo) / (hi - lo), 0, 1)`: 1 within `width / 2` of the offset edge,
falling over one pixel (the antialiased edge) plus the softness across each side. The band is the
colour premultiplied by its alpha times `min(width, 1)` (a band thinner than a pixel is a one-pixel
line that fades with its width), times `k`; the raster keeps it premultiplied, so HDR colour is kept.
Over composites the band over the input, Under the input over the band, Contour only publishes the
band alone. Width, softness and offset are clamped to their domains first.

The distance transform covers the input's data window grown by its reach, `|offset| + h + 1/2 +
softness / 2 + 2` pixels from constant operands, or the widest band the domains allow when any of
them is animated or driven, so a band never meets the transform's clamped distance. The output's data
window is that grown window, and a reduced render scale reads the distance in full-resolution pixels,
so the band keeps its width in the composition. The transform is the cost: two separable passes over
the grown window on the CPU, a bounded search on the GPU; the raster is one pixel pass.

Of Path Stroke's controls only the width, softness (of the edge) and offset apply: trim, dashes,
tapers, brush stamps and wiggle are measured along a vector path, and an alpha edge is not one. A path
is never traced from the pixels, which would read the frame back mid-frame (path-processing.md, "Where
a path comes from: the boundary"); draw a vector path with Mask Path, Text Outline or a Shape for those.

**Evidence.** The direct recipe test compares the chain at 1e-4 with an independent reference (a
brute-force Euclidean distance between pixel centres, the band and the composite) for Over, Under
with a hard edge, an inward hairline alone, and a wide soft HDR band over a solid; and checks that
width 0 is the input, the inside beyond the band is the input exactly, Contour only is transparent away
from the edge and carries the colour on it, a negative offset moves the band inside, the band keeps its
width at half render scale, an animated width equals the constant one at its key, an ROI equals the
full frame, cancellation inside the transform publishes nothing and an exhausted budget refuses the
frame. The compiler test reads the three operations, the reach, the program and its image slot.
`node.bloom.alpha-contour` prepares through the production builder (full frame and ROI); the native
harness holds three variants to the CPU oracle (2e-6; the band's alpha comes from a square root the
device rounds within an ulp, so alpha is held to the same 2e-6, not bit-exact) with warm reuse and a
changed colour re-running only the raster; and `bloom.runtime.gpu_path_processing_native` renders two
Alpha Contours over a curved Shape Source and a Text Source through the document route.
[Benchmarks](../development/benchmarks.md) lists `alpha-contour-2k`, written and not yet run.

## Histogram

`bloom.histogram`, schema 1, under Tracking & Analysis (issue #349), has one Image in, `input`, and one
Image out, `image`: a bar graph of how many pixels of the input's data window have each value. It takes
no effect mask, because it publishes a graph and not the input changed. It lowers to the three
`HistogramKernel` passes below, which share one grade kind (`ColorGradeKind::Histogram`) and one GPU
program (`bloom.render.histogram.v1`).

| Role | Default | Domain |
|---|---|---|
| `channels` | Red, green and blue | Red, green and blue; Luminance; Red, green, blue and luminance; Red; Green; Blue; Alpha |
| `space` | Straight color | Straight color, Stored values |
| `rangeMin`, `rangeMax` | 0, 1 | -1,000,000 to 1,000,000; keyable and drivable |
| `bins` | 256 | 64, 128, 256, 512 |
| `height` | 128 | 64, 128, 256, 512 (the graph's height in pixels) |
| `graphScale` | Linear | Linear, Logarithmic |

**What is counted.** `bins` equal bins span `rangeMin` to `rangeMax`; a value is counted in bin
`floor((v - rangeMin) * (bins / (rangeMax - rangeMin)))`, both ends of the range being in (the top end
is the last bin). A value outside the range, or not finite, is not counted, and a reversed or empty
range counts nothing. With Straight color the red, green and blue are divided by alpha first, and a
pixel without alpha has no color and counts in none of the color channels (Alpha counts every pixel);
with Stored values the premultiplied values are counted as they are. Luminance is the Rec. 709 weights
`0.2126 r + 0.7152 g + 0.0722 b` of that color, added in that order. The input's data window is
counted whole, whatever region of interest is rendered, so the graph does not change with the Viewer's
region.

**The graph** is `bins` wide and `height` tall at the input's origin, on a scene-linear background of
0.02 with an opaque alpha. A channel's bar at a bin has the height `count / tallest * height`, where
the tallest is the largest count of any counted channel and bin (Logarithmic: `log2(1 + count) /
log2(1 + tallest)`), and the row it ends in is covered by its fraction. Red, green and blue bars are
drawn in their own colors; luminance and alpha in gray; the channels are added over the background, so
overlapping bars of the three channels go to white. With nothing counted the graph is the background.

**The passes.** `Bin` cuts the frame into at most 32 by 32 tiles (cut as Auto Exposure's
reduction cuts its frame) and counts each tile's pixels into bins, three bins to a pixel (an image's alpha is limited
to [0, 1], so the counts use red, green and blue): one row of the result per tile, one pixel per group
of three bins and counted channel. `Sum` adds the tiles' rows, in tile order, into one row. `Plot` draws
the graph from that row. Each pass is its own operation, so a frame's counts are shared by every
graph setting that reads the same pixels and range, and a changed `graphScale`, `height` or color choice
re-runs only what it changes. A frame reads nothing but its own pixels: reaching it by playing,
scrubbing or exporting gives the same graph, there is no state carried between frames, and a change
to the input's pixels, the range, the channels or the space changes the passes' identity and so the
graph.

**On the CPU and the GPU.** `applyHistogram` and the GPU program run the same binary32 operations in
the same order over the same table (`-ffp-contract=off`; the program's arithmetic is `precise`). The
counts are whole numbers below 2^24 in `Bin`, and `Sum` adds them in a fixed order, so the two agree
exactly; the plot's division is correctly rounded on both and only a logarithmic scale takes each
backend's own `log2`, which agrees to within its last bits (2e-6 relative on color, alpha exact). The
graph stays on the GPU: the three passes are resident dispatches over the frame and its counts, the
tile counts are never read back, and the only readback is the composition's final image.
Because a GPU has no atomic counter here, each `Bin` pixel counts its tile's pixels against its own
three bins; the work is therefore about the bin count times the frame's pixels (a suspicion of cost,
not a measurement; [Benchmarks](../development/benchmarks.md) lists `histogram-compare-2k`).

**Evidence.** `src/runtime/tests/histogram_compare_tests.cpp` reads counts and bar heights from real
pixels (bin edges, HDR and negative values, straight and stored values, luminance, alpha, a window not
at the origin, tile independence against an independent count at five image sizes, linear and
logarithmic bars, the refusals, the pixel budget and cancellation); `snapshot_compiler_effect_tests.ipp`
holds the lowering; the native harness (`runHistograms`) holds every channel choice, scale, range and
a source smaller than its tile counts to the CPU oracle with cold, warm and changed-value runs and three
resident dispatches with no per-operation readback; `effect.HistogramKernel` and `node.bloom.histogram`
prepare through the production builder in the coverage contract at full frame and ROI.

## Image Compare

`bloom.image-compare`, schema 1, under Tracking & Analysis (issue #349), has two required Images,
`input` (A) and `second` (B), and one Image out, `image`, over A's data window; B is transparent black
where it has no pixels. It takes no effect mask. It lowers to the `PixelMathKernel` of Image Math
(`PixelMathOperation::Compare`) and so shares its program, its atlas for the second image and its
table, which is 36 floats and 44 on the GPU (the second image's offset, then the dispatch window's
place in the display window and the display window's size).

| Role | Default | Domain |
|---|---|---|
| `mode` | Difference | Difference, Split, Checkerboard |
| `gain` | 1 | 0 to 1,000,000 (Difference) |
| `splitPosition` | 0.5 | 0 to 1 (Split) |
| `angle` | 0 | -360 to 360 degrees, with a dial (Split) |
| `cellSize` | 64 | 1 to 4,096 pixels, rounded to a whole pixel (Checkerboard) |

**Difference** is `|A - B| * gain` on the stored (premultiplied) red, green and blue, with the larger
of the two alphas; a result that is not finite is 0. **Split** shows A on one side of a line and B on
the other: with the normal `n = (cos angle, sin angle)` in display pixels (angle 0 is a vertical line
with A on the left, 90 a horizontal one with A above), a pixel center `p` shows A when
`p . n < c . n + (position - 0.5) * (|n.x| * width + |n.y| * height)`, where `c` is the display
window's center and `width` and `height` are its size, so `splitPosition` 0 shows B everywhere and 1 shows
A everywhere. **Checkerboard** shows A where `floor(x / cell) + floor(y / cell)` is even, from the
display window's corner. Split and Checkerboard are laid on the display window, not the data window, so
the same pixel shows the same image whatever region or window is rendered.

**On the GPU** the second image is placed beside A in the atlas as for Image Math and one dispatch
computes inside the output window. Difference, the split's line test and the squares are additions,
subtractions, multiplications and integer arithmetic in the same order as the CPU reference, so the
two agree exactly (the table's cosine and sine are computed once, on the host, for both).

**Evidence.** `histogram_compare_tests.cpp` reads Difference, the split at positions and angles, the
squares, a second image of another window, a clipped data window against the full one, the refusals
and the table's identity; the native harness (`runPixelMaths`) runs every mode against a second image
of the same, a smaller and a larger window to the CPU oracle; `node.bloom.image-compare` and
`effect.PixelMathKernel` prepare through the production builder at full frame and ROI.

## Image Statistics

Not built. Histogram and Image Compare are image nodes whose result stays on the GPU. Image
Statistics is meant to give the value graph numbers (a minimum, a maximum, a mean) that drive other
nodes, for instance a Map Range that normalizes a depth pass. That is not an image: the number would
have to come from the frame's pixels. The value graph is evaluated before the image pass, and Layer
Bounds is read after it only because bounds need no pixels. A GPU frame is rendered without
returning any value to the host, and a value read back in the middle of a frame is neither the one
final readback of the coverage contract nor a host-preparation step it lists. Making it so is a
decision for the coverage contract and the GPU frame loop, not an addition to a node, and it also
needs the image pass to follow a value that depends on its own pixels. The reduction by tiles of the
Histogram's `Bin` and `Sum` passes (and of Auto Exposure's) is the part that carries over.

## Lens Distortion

`bloom.lens-distortion`, schema 1, under Distort (issue #486), has one Image in, `input`, and two Image
outputs: `image`, the warped picture, and `stMap`, the same warp as an ST map. It takes no effect mask.
Undistort and Redistort are inverses of each other.

| Role | Default | Domain | Meaning |
|---|---|---|---|
| `mode` | Undistort | Undistort, Redistort | Which way the lens model is applied (constant) |
| `k1` | 0 | -2 to 2 | Radial coefficient: negative is barrel, positive pincushion |
| `k2` | 0 | -2 to 2 | Radial coefficient that grows faster towards the corners |
| `p1` | 0 | -0.5 to 0.5 | Tangential (decentring) coefficient |
| `p2` | 0 | -0.5 to 0.5 | Tangential (decentring) coefficient |
| `center` | 0, 0 | within 1e6 px | The lens centre, pixels from the composition's centre with y down |

Every value except `mode` has a socket and can be keyed or driven.

**The model.** Brown, "Decentering Distortion of Lenses" (Photogrammetric Engineering 32(3), 1966), after
Conrady, "Decentred Lens-Systems" (Monthly Notices of the Royal Astronomical Society 79, 1919): the
Brown-Conrady model with two radial and two tangential terms, in the form every common lens calibration
uses. Coordinates `(x, y)` are normalised about the centre by half the composition's diagonal (a corner of
the frame is at `r = 1` for a centred lens, whatever the frame's shape), and `r2 = x x + y y`:

```text
s    = 1 + k1 r2 + k2 r2 r2
D(x, y) = ( x s + 2 p1 x y + p2 (r2 + 2 x x),
            y s + p1 (r2 + 2 y y) + 2 p2 x y )
```

`D` takes an undistorted position to the distorted one the lens put it at. Each output pixel reads the
input at one position, found from the pixel's own: **Undistort** reads `D(p)`, the picture the lens
would have to have produced; **Redistort** reads `D^-1(p)`, which puts the lens's distortion onto a
straight picture. The two are inverses wherever `D` rises monotonically from the centre, which holds
across the frame for ordinary lens values; far outside that (a `k1` below -1 on a centred lens, say) the
inverse has no solution and Redistort is not defined there. `D^-1` is found by twelve Newton steps from
the pixel's own position over `D`'s analytic Jacobian, each step limited to one normalised unit, and
costs about twelve times what Undistort does per pixel.

**The ST map.** `stMap` holds, in red and green, the position each pixel reads, normalised over the
composition frame: 0, 0 is the frame's bottom left and 1, 1 its top right, blue is 0 and alpha is 1. It is
the data an ST-map warp reads, so the same warp can be applied to another picture. It is raw data,
scene-linear, and the frame stands for the composition whatever the input's size, so a proxy draws the
same values at fewer pixels. An output nothing reads is not built.

**Lowering.** The node lowers to a field raster (`docs/architecture/fields.md`) per output the graph reads,
each over the input's data window. The program is `Coordinate`, one `Lens Warp` instruction holding the
model, and for `image` an `Image` instruction reading the input at the warp's position, bilinear over its
premultiplied texels, transparent outside the input, result kind Premultiplied (a texel is kept exactly as
stored, colour at zero alpha included). For `stMap` the position is scaled by `1 / frame size` and moved by
one half. The single `Lens Warp` instruction is in `src/core/field_kernel.glsl`, the one kernel the CPU
reference and the GPU program both run, so the two share every operation and rounding. With all four
coefficients constant zero the `image` output is the input itself and the node adds no operation; a muted
node is the same, and its `stMap` is the identity map.

**HDR and alpha.** The image is read, never remapped through colour: bilinear sampling of premultiplied
texels is linear in the light, so values above one, negative values and partial alpha keep their range and
their ratio, and a pixel outside the input is transparent black. Nothing is divided by alpha. Because
the raster reads the input wherever the warp points, a region of interest or a proxy gives the whole
frame's pixels (the input is required whole behind a region, as for Transform 2D).

**On the GPU.** The field raster's program, unchanged: the input is moved beside the canvas in an atlas,
one dispatch of the fixed field program reads it at the warp's positions, and the canvas is cut back out.
Nothing is read back. A coefficient or the centre changes only the program's data, not the program, so
changing one re-dispatches without recompiling.

**Evidence.** `src/core/tests/field_program_tests.cpp` holds the Lens Warp instruction to an independent
double-precision reading of the model, Undistort and Redistort as inverses of each other, the identity at
zero, the centre fixed, barrel against pincushion, and the premultiplied selector keeping colour at zero
alpha. `src/runtime/tests/snapshot_compiler_lens_distortion_tests.ipp` holds the lowering (which
operations, the program, the operands, mute, identity, time dependence) and, on the CPU reference over a
checker grid, the ST map against an independent reading of the model in both modes at two proxy sizes,
Redistort as the inverse of Undistort at the node, the warped image as the input read bilinearly at the
ST map's positions, HDR and partial alpha kept, transparency outside the input, a region of interest equal
to the whole frame's pixels, the pixel budget and cancellation. `effect.FieldRasterKernel` and
`node.bloom.lens-distortion` prepare through the production builder at full frame and ROI, and the native
harness holds the node to the CPU oracle on a device. [Benchmarks](../development/benchmarks.md#lens-distortion-486)
names the scenario and the suspected cost.

## Chromatic Aberration

`bloom.chromatic-aberration`, schema 1, under Distort (issue #486), has one Image in, `input`, and one Image
output, `image`: the picture with red and blue pulled apart along the line from a centre, by an amount
that grows with the distance from it, and green left where it is. It takes no effect mask.

| Role | Default | Domain | Meaning |
|---|---|---|---|
| `amount` | 0 | -0.2 to 0.2 | The offset of red and blue as a fraction of half the composition's diagonal, reached at that distance from the centre; negative swaps the two |
| `center` | 0, 0 | within 1e6 px | The point the colours spread from, pixels from the composition's centre with y down |

Both have a socket and can be keyed or driven. The default amount is 0, so a new node leaves the picture
exactly as it is.

**The model.** Coordinates `(x, y)` are normalised about the centre by half the composition's diagonal (as
for Lens Distortion), and `r2 = x x + y y`. Red is read at `(x, y) (1 + amount r2)`, blue at
`(x, y) (1 - amount r2)` and green at `(x, y)`: the radial term of the Brown-Conrady lens model
(`k1 = amount`) forward for red and mirrored through the pixel for blue, so red and blue move the same
distance in opposite directions. The offset is `amount r2` times the distance from the centre, so it is
nothing at the centre and grows quickly towards the edges; a frame corner of a centred pad is at `r = 1`.
Because the offset is measured in half diagonals, an amount means the same on a proxy as at full size.

**Lowering.** One field raster (`docs/architecture/fields.md`) over the input's data window. The program is
`Coordinate`, one `Lens Warp` instruction (mode 0, `k1` the amount, the other coefficients constant zero) for
the red position, the blue position `2 p - red` (a Swizzle and a Subtract), three `Image` instructions
reading the input at the red, green and blue positions (bilinear over premultiplied texels, transparent
outside the input, selector 3, the texel as stored), a Swizzle each to bring green and blue to the first
lane, two Max instructions and a Swizzle for the alpha, and a Combine of red, green, blue and alpha into a
Premultiplied result. It adds no field instruction: the Lens Warp, the Image selector and the Premultiplied
result kind are Lens Distortion's, in `src/core/field_kernel.glsl`, the one kernel the CPU reference and the
GPU program both run. With the amount a constant zero the output is the input itself and the node adds no
operation; a muted node is the same.

**HDR and alpha.** Each channel is taken from the premultiplied texel at its own position and nothing is
divided by alpha or clamped, so values above one and negative values pass through in proportion and a
picture with no fringe to make (a flat colour) is unchanged. The output's alpha is the largest of the three
alphas, so a channel is never cut off by an alpha that is smaller than the one it was read with; at an edge
of partial alpha the fringe is a colour at the alpha of the widest of the three reads. Outside the input a
read is transparent black. Because the raster reads the input wherever the offset points, a region of
interest or a proxy gives the whole frame's pixels (the input is required whole behind a region).

**On the GPU.** The field raster's program, unchanged: the input is moved beside the canvas in an atlas,
one dispatch of the fixed field program reads it three times, and the canvas is cut back out. Nothing is
read back. The amount and the centre change only the program's data, not the program.

**Evidence.** `src/runtime/tests/snapshot_compiler_chromatic_aberration_tests.ipp` holds the lowering (which
operation, the program, the operands, mute, identity, time dependence) and, on the CPU reference over a
checker grid, each of the four output channels against an independent double-precision reading of the
input, green untouched, the colours pulled apart, HDR and partial alpha kept, a region of interest equal
to the whole frame's pixels, the pixel budget and cancellation. `node.bloom.chromatic-aberration` prepares
through the production builder at full frame and ROI, and the native harness holds the program to the CPU
oracle on a device. [Benchmarks](../development/benchmarks.md#chromatic-aberration-486) names the scenario
and the suspected cost.

## Light Rays

`bloom.light-rays`, schema 1, under Light & Shadow (issue #508), has one Image in, `input`, and one Image
output, `image`: the picture with volumetric rays added that radiate from a point. Bright (or opaque)
parts of the picture streak away from the point, tinted and boosted, on top of the picture. It takes no
effect mask.

| Role | Default | Domain | Meaning |
|---|---|---|---|
| `source` | Luminance | Luminance, Alpha | What drives the rays: the picture's Rec. 709 luma, or its alpha. A constant with no socket |
| `center` | 0, 0 | within 1e6 px | The point the rays radiate from, pixels from the composition's center with y down |
| `length` | 50% | 0 to 100% | How far each pixel gathers towards the center, as a fraction of its distance to it; 100% reaches the center |
| `falloff` | 1 | 0 to 8 | How fast a ray fades along its length; 0 weighs it evenly |
| `boost` | 1.5 | 0 to 100 | A multiplier on the gathered rays |
| `tint` | white | channels 0 to 100 | The color the rays are multiplied by; white leaves the source's own color. A constant with no socket |

`center`, `length`, `falloff` and `boost` have a socket and can be keyed or driven. The source and the
tint are constants: the field program takes scalar and vector operands, so a color is part of the
program rather than an animated operand. Length 0 or boost 0 leaves the picture exactly as it is.

**The model.** Each pixel `p` gathers the source along the segment from `p` towards the center `c`, the
fraction `length` of the way. With `n = min(floor(reach / 2) + 1, 96)` samples, `reach` the segment's length
in pixels, sample `i` lies at `q = p + (c - p) length t` for `t = (i + 1/2) / n`, reads the bilinear
premultiplied texel's luma (or alpha, transparent outside the picture) and is weighed `(1 - t)^falloff`.
The rays are the weighted mean of those reads times the boost, `m`. The pixel's result is the input's own
premultiplied texel plus `m` times the tint in the color channels and `min(m, 1)` in alpha (alpha is
then clamped to 1). A pixel that is not itself lit therefore receives the light of whatever lies between it
and the center, which streaks it away from the center; the farther a source is along a pixel's segment the
less it counts. The sample count follows the segment's length, one per two pixels, so a short ray takes few
samples and the picture does not depend on the proxy except through the resolution of its reads. The
center is in composition pixels, and `length` is a fraction, so both mean the same on a proxy as at full
size.

**HDR and alpha.** Nothing is clamped before the sum: luma above one gives rays above one, and the input's
values pass through. The rays are added as light, so an area that is already opaque stays opaque and a
transparent area with rays in front of it gains alpha up to `m`. The gather stays inside the input's data
window: rays reach as far as the picture does and no further, and a source outside it is transparent.

**Lowering.** One field raster (`docs/architecture/fields.md`, "Rays") over the input's data window: `Coordinate`,
one `Rays` instruction (the source as its constant choice; the length, falloff, boost and center its
operands), a `Constant` for the tint, a `Math` multiply, an `Image` read of the input at the pixel
(selector 3, the texel as stored) and a `Math` add, into a Premultiplied result. With a constant length
or boost of zero the output is the input itself and the node adds no operation; a muted node is the same.
A raster that gathers an image is evaluated over the whole image behind a region of interest.

**On the GPU.** The field raster's program with the new instruction: the input is moved beside the
canvas in an atlas, one dispatch of the fixed field program marches the rays, and the canvas is cut back
out. Nothing is read back. The kernel source is the CPU reference's, so the two take the same samples with
the same arithmetic.

**Evidence.** `src/core/tests/field_program_tests.cpp` holds the gather against an independent
double-precision reading (both sources, falloffs, lengths) and the identities and refusals of the encoder;
`src/runtime/tests/snapshot_compiler_light_rays_tests.ipp` holds the lowering (which operation, the
program, the operands, mute, identities, time dependence) and, on the CPU reference over a checker grid,
the whole picture against an independent gather, the input kept under the rays, a region of interest equal
to the whole frame's pixels, the pixel budget and cancellation. `node.bloom.light-rays` prepares through
the production builder at full frame and ROI for both sources, and the native harness holds the program to
the CPU oracle on a device. [Benchmarks](../development/benchmarks.md#light-rays-508) names the scenario and
the suspected cost.

## Rolling Shutter

`bloom.rolling-shutter`, schema 1, under Distort (issue #568), has one Image in, `input`, an optional Image
in, `motion`, and one Image output, `image`: the picture as a sensor that reads its rows one after another
would have recorded it. It takes no effect mask.

A rolling shutter reads the frame over a time, not at an instant: the row read at phase `s` (-1/2 at the
first row read, +1/2 at the last) shows the scene `s * readout` frames after the frame's time. Whatever
moves while the sensor reads is skewed, and a motion that changes during the read (a shake) wobbles.

| Role | Default | Domain | Meaning |
|---|---|---|---|
| `readout` | 0.5 | 0 to 1 | The time the sensor takes to read the frame, a fraction of a frame; 0 is a global shutter. A constant: it fixes the moments the camera is read at |
| `direction` | Top to Bottom | 0 to 3 | The order rows (or columns) are read: Top to Bottom, Bottom to Top, Left to Right, Right to Left. A constant |
| `edges` | Extend | 0 or 1 | What a read past the picture's edge sees: the edge pixel (Extend) or transparent. A constant |
| `amount` | 1 | 0 to 4 | Scales every motion below; 1 is the motion as it is. Socket, keyable and drivable |
| `cameraOffset` | 0, 0 | within 1e6 px | A Camera Shake's Offset output. Socket and driver |
| `cameraRoll` | 0 | within 1e6 | A Camera Shake's Roll output. Socket and driver |

**Motion sources.** The node skews by whichever of the two it is given, and by both if both:

- **Camera Shake, exactly.** Connect a Camera Shake's `offset` to `cameraOffset` and its `roll` to
  `cameraRoll` (both from the same shake), the shake being upstream of the picture. The value pass then
  makes three copies of the shake's value operation (`CompiledValueShake` with `timeOffsetFrames`
  -readout/2, 0 and +readout/2 and `components`, publishing offset x, offset y and roll as three scalars),
  so the node reads the shake's own motion (`core/camera_shake.hpp`) at the first, the middle and the last
  moment of the readout: not an estimate from the pictures. The shake is a pure function of its seed and
  the time, so this needs no earlier frame. Any other source on those two sockets (a keyed value, another
  node's output, a different Camera Shake for each socket) is refused with a named diagnostic, because only
  a Camera Shake can be read at another moment; a typed constant is a camera that does not move during the
  readout and does nothing.
- **Optical flow.** Connect an Optical Flow's `forward` image to `motion`. The flow is read at each pixel
  (bilinear, edge pixel beyond the picture): its red and green are the velocity in pixels per frame, y down.

**The model.** With the shake's samples `a`, `b`, `c` of offset (and roll) at the three moments, the change
since the frame's time at phase `s` is the quadratic through them, `d(s) = s (c - a) + 2 s^2 (c + a - 2 b)`,
exact for any motion up to quadratic over the readout and within a fraction of a pixel for Camera Shake's
motion (a test holds it to half a pixel). The input already carries the shake at the frame's time, so the pixel at
`p` reads the input at `q`, with `v = p - o(b) - d_o`, `q = v + g perp(v) + o(b)`: `d_o` the offset's change,
`g` the roll's change in radians (clockwise), `perp(x, y) = (-y, x)` and positions measured from the
composition's center; this is the inverse of the shake's move to first order in the roll's change over the
readout, and it ignores the shake's overscan scale. The flow adds `q -= f s readout amount`. Everything
is multiplied by `amount`. The field raster works in y up, the shake and flow in y down, so the y of each is
flipped. `s` is measured in composition pixels (`-y / height`, `y / height`, `x / width`, `-x / width` for
the four directions), so a proxy computes the same phases.

**Lowering.** One field raster (`docs/architecture/fields.md`) over the input's data window, program
`Coordinate`, the phase (a Swizzle), the constants of the three samples, the quadratic (Add, Subtract,
Multiply and Swizzle instructions), the optional flow read (`Image` instruction, selector 1, bilinear, Extend)
and the final premultiplied `Image` read of the input; no new field instruction. A constant amount of zero, a
zero readout, no connection on either camera socket with no flow, and a muted node are the identity and
add no operation. The Camera Shake is a pure function of the seed and the time, so any frame renders on its
own and the same in preview, scrubbing and export; the node carries no state between frames and its cache
identity is the program with the operands (the three samples' outputs, the amount) and the frame time. The
raster reads the input wherever the position points, so a region of interest or a proxy gives the whole
frame's pixels, and CPU work checks cancellation per row, reports row progress and refuses an output past
the remaining pixel budget.

**On the GPU.** The field raster's program, unchanged: the picture and the flow are moved beside the canvas
in the atlas, one dispatch of the fixed field program, nothing read back, the operands changing only the
program's data. `node.bloom.rolling-shutter` is a Required contract entry satisfied by the production
preparation of its lowering's program (full frame and ROI), and the native harness holds the same program to
the CPU oracle on a device.

**Evidence.** `src/runtime/tests/snapshot_compiler_rolling_shutter_tests.ipp`: the lowering (the identities,
the three shake copies and their offsets, the program against the coverage fixture's, time dependence),
and on the CPU reference over a checker grid each pixel against an independent double-precision reading of
the input through the quadratic of the shake's three samples, the quadratic's gap from the shake's exact
motion, a flow image skewing by phase, a region of interest, the pixel budget and cancellation.
`testRollingShutterDirectionsAndEdges` holds all four read directions (Top to Bottom, Bottom to Top,
Left to Right, Right to Left), at a readout of 0.5 and of 1.0, shake-driven and flow-driven, with
Transparent and with Extend edges, to 3e-4 against an independent double-precision reading that takes the
phase of each direction from the composition pixels (`-y/height`, `y/height`, `x/width`, `-x/width`); the
motion is large enough that rows or columns read past the picture, and the test asserts that a read
entirely past the edge is transparent (Transparent) or the edge pixel (Extend), that the two modes agree
exactly wherever no tap left the picture, that a horizontal read skews by the column's phase, and that a node
that does not name its edges is Extend. The native harness holds the lowering's program for all four
directions and both edge modes to the CPU oracle on a device (a test checks each program is the lowering's
instruction sequence).
[Benchmarks](../development/benchmarks.md#rolling-shutter-568) names the scenario and the suspected cost.

## Auto Exposure

`bloom.auto-exposure`, schema 1, under Color Correction (issue #566), has one Image in, `input`, one Image
out, `image`, and the optional effect mask every filter takes. It measures the brightness and the color
of the footage and applies a gain that follows them with a lag, the way a phone camera hunts when a light
dims or flares. One node does both corrections because they share one measurement per frame and one
look-back window; set **Exposure** or **White balance** to 0% to use the other alone. Every value except the
look-back has a socket and can be keyed or driven:

| Role | Default | Domain | Meaning |
|---|---|---|---|
| `exposure` | 100% | 0 to 100% | How much of the exposure correction is applied |
| `target` | 0.18 | 0.01 to 4 | The scene-linear level the frame's mean luminance is brought to |
| `attack` | 0.25 s | 0 to 30 s | The time constant of the correction following the footage getting brighter (the gain falling); 0 follows at once |
| `release` | 0.75 s | 0 to 30 s | The same for the footage getting darker (the gain rising) |
| `minGain` | -3 st | -8 to 0 | The most the exposure gain may fall, in stops |
| `maxGain` | +3 st | 0 to 8 | The most the exposure gain may rise, in stops |
| `whiteBalance` | 100% | 0 to 100% | How much of the white balance correction is applied |
| `balanceLag` | 1 s | 0 to 30 s | The time constant of the white balance following the footage's color |
| `balanceLimit` | 1 st | 0 to 4 | The most the white balance may move red or blue against green, in stops |
| `lookBack` | 48 f | 4 to 64 | The frames the node looks back over, counting the current one; a constant with no socket |

There is no seed: nothing in the node is random.

**The look-back window.** Every frame is computed from the window of `lookBack` frames that ends on it, and
from nothing a previous render left behind, so a frame is the same played to, scrubbed to, reached backwards,
exported, and rendered by the scripted and headless routes. Sample `i` of the window is the input on the
frame `max(f - (lookBack - 1 - i), 0)` of the composition's frame rate, `f` being the operation's clock
frame as a Time node counts it: a frame near the start repeats the first frame, so the window never reads
before the composition. The last sample is the input itself. Each is a `TimeSampleKernel` with `lookBack`
set (`lagFrames` the distance back), built by `appendLookBackSamples` in `look_back_recipes.cpp`, which is
the one routine that builds a bounded set of earlier frames as time samples on the frame grid and is meant
for any node that reads the frames around its own. The window is at most 64 frames
(`kMaximumLookBackFrames`); a larger one does not lower and the plan is refused with a diagnostic. The
operation cache keys each sample's result on its input's content at that source time, so a frame whose window
reaches a time another frame already measured shares that measurement: in playback or an export a frame
measures one new source frame, and a cold frame (a scrub or a jump) measures the whole window.

**The measurement.** Two passes of `AutoExposureKernel` in the Reduce role reduce each sample to its mean
color. The first cuts the sample into at most `64 x 64` tiles (`tileWidth = ceil(width / min(64, width))`,
likewise for the height, so a tile is the same size but for the last row and column) and, for each tile,
sums the premultiplied red, green and blue in row-major order in binary32 and multiplies by `1 / (width *
height)`. The second sums that grid to one pixel. The result is the mean of the premultiplied color over
the sample's data window, a transparent pixel counting as black, with alpha 1; it lies at the sample's
origin. The grid, and so the work of the second pass, is bounded however large the frame is, and the
order of the sums is fixed, so the CPU reference and the GPU add the same numbers in the same order. A
Reduce reads its whole input, never a region of interest (the evaluator marks it full-support), so a mean
does not depend on the region asked for; under a proxy it is taken at the proxy's resolution, which
differs from the full-resolution mean only by what the resampling does to the mean.

**The filter.** One `Filter` operation reads the window's means, oldest first (its input is the newest;
`fieldImages` hold the older), and publishes one pixel whose red, green and blue are three gains in stops:
the exposure `e`, and the red `wr` and the blue `wb` against green. Each mean `m` first gives the gains it
asks for, with `r, g, b = max(m, 1e-6)`:

```text
wr = clamp(log2(g / r), -limit, limit) * whiteBalance
wb = clamp(log2(g / b), -limit, limit) * whiteBalance
luma = 0.2126 r 2^wr + 0.7152 g + 0.0722 b 2^wb        (Rec. 709, after the white balance)
e  = clamp(log2(target) - log2(max(luma, 1e-6)), minGain, maxGain) * exposure
```

The first mean that exists seeds the state with its own gains. Each later mean `d` moves it by a one-pole
recursive filter, `s = s + k (d - s)`, with the coefficient of a lag `tau` seconds sampled every
`dt = 1 / frameRate` seconds, `k = 1 - exp(-dt / tau)` (1 for `tau = 0`). The exposure uses the attack
coefficient where `d.e < s.e` (the gain is falling, the footage got brighter) and the release otherwise;
the two white balance gains use the balance lag. A mean that does not exist (the layer has no pixels at that
time) holds the state and is not taken for black.

The final state still carries the seed with the weight `p`, the product of `(1 - k)` over the steps, with
the `k` actually applied at each (so the attack and the release stay asymmetric), one `p` for each of the
three gains. When a change leaves the window that weight would vanish at once and the correction would
pop, so the seed's share is taken out: `result = (s - p seed) / (1 - p)`. A constant input returns that
constant and a coefficient of 1 (no lag) still returns the newest gain exactly. A `p` of 1/256 below one
or more (`kAutoExposureSeedWeightLimit`, 0.99609375: a lag far longer than the window, or a step of no
length) leaves the seed nearly the whole state, which cannot be separated without dividing by a vanishing
remainder, so there the state itself is the result. The result is a lag-weighted mean of the window's
means; a lag longer than about a third of the look-back is cut short, since the frames before the window
are not seen, but it settles without a jump. The defaults (a release of 0.75 s, a balance lag of 1 s, a
look-back of 2 s at 24 f/s) keep the lags inside it. The GLSL computes the same thing in the same order,
with the division correctly rounded.

**The gain.** The `Apply` role multiplies the input's premultiplied red, green and blue by `2^(e + wr)`,
`2^e` and `2^(e + wb)` from the Filter's pixel and keeps alpha. With no Filter pixel it leaves the input
exactly as it is, and with 0 stops it is exact too. The Filter's pixel is not a color: its channels hold
stops, its alpha is 1.

**The plan.** The compiler lowers the node to, for each frame of the window, one time sample (but the last)
and two Reduce passes; then the Filter, carrying the node's nine values (each value's identity on it, so an
animated value stays animated); then the Apply on the input. The operations are marked as varying with
time through the input's and the values'. The look-back is a constant: a change to it is a new plan, and
a change to a value is new data for the Filter alone, so the measurements are not recomputed.

**On the GPU.** One fixed resident program (`ColorGradeKind::AutoExposure`) with every value in its table.
A Reduce is dispatched over the sample's own window and the pass publishes the part of it that holds the
grid, cut out at the origin (one exact move). A Filter and an Apply read more than one image the way Z
Defocus's gather does: an atlas, the followed image padded to it and each other image moved into place
(one exact move) and blended over it in Normal mode; the Filter's is a row of one pixel for each mean
and the Apply's the frame with one more column for the gain, and the result is cut back out. The arithmetic
is additions, multiplications, min, max, correctly rounded divisions and the logarithm and the exponential
of base two, in one fixed order. The sums are therefore the same bit for bit; the logarithm and the
exponential are each device's own, so the gains agree to within their last bits, not exactly, and the tests
hold them to the strict 2e-6 gate. Nothing is read back but the final frame.

**Cache, budget, cancellation, retirement.** The identity of each pass is its input's content and its
prepared table (the pass, the grid and, for the Filter, the values and the coefficients), so the sample
measurements of a source time are shared and a changed value invalidates only the Filter and the Apply. The
outputs of a Reduce are at most 64 by 64 pixels and the Filter's is one, so the window adds almost nothing
to the cache; the Apply is the frame's size and is charged against the pixel budget, and a Reduce is charged
for the dispatch over its input. A refused budget is a diagnostic from the pass that refused it
(`PixelStorageBudgetExceeded` on the CPU, a preparation failure on the GPU), never a frame computed from
fewer samples. Every pass checks the cancellation token at its row bands, a cancelled pass publishes
nothing, and the images are ordinary operation-cache and resident-GPU entries, retired with them.

**Limits.** The measurement is the mean of the whole frame: a small bright light moves it little, and the
node has no metering region (use the effect mask to limit where the gain shows, not where it measures).
A cut in the footage is followed with the lag like any other change. A lag over a third of the look-back is
cut short as above. A cold frame costs a whole window of the input, so a slow input makes scrubbing slow.
No speed has been measured: [Benchmarks](../development/benchmarks.md#auto-exposure-566) lists the scenario
that will, and no budget is set.

**Evidence.** `src/runtime/tests/auto_exposure_tests.cpp` holds the reduction on every grid shape against an
independent mean, the filter against an independent double-precision reading of this section (steady,
attack, release, limits, amounts, no lag, absent means), the gain, the table and its one program, the
pixel budget, cancellation and row bands, and, over whole plans, a frame independent of how it is reached,
the response of a flare and a dimming light, a warm shot made neutral, the reuse of measurements between
frames by cache identity and a refused budget as an explicit error. The native harness (`runAutoExposures`)
holds a flare, a dimming light, a warm shot, a window reaching before the first frame, a proxy and a region
of interest to the CPU oracle at the strict gate with exact alpha, with resident dispatches and no
readback, warm RAM reuse with zero dispatches, the next frame sharing the measurements and a changed value
re-running. `effect.AutoExposureKernel` and `node.bloom.auto-exposure` prepare through the production
builder at full frame and ROI. `testAutoExposureLowering` holds the lowering.

## Matte Workflow

Issue #383 lets any node that produces a Mask (Cryptomatte, the Mask node, Matte Blur, Luma Key,
a field rasterized to a Mask, and the two nodes below) shape a layer and limit an effect, and adds
the conversions between images and Masks. A Mask is ADR 0028's transport: premultiplied white with
the coverage in all four channels. Nothing here has a pixel kernel of its own: every route lowers to
the existing Channel Select, Multiply Alpha and Image Mix (with its Mask) combines, so the CPU
reference, the GPU dispatch, cache identity, invalidation and cancellation are exactly those
kernels'. The GPU coverage contract requires `node.bloom.mask-combine`,
`node.bloom.image-to-mask`, `feature.mask.combine`, `feature.mask.effect_input`,
`feature.mask.layer_matte` and `feature.matte.track_node`, each a production-prepared plan.

### Mask Combine

`bloom.mask-combine`, schema 1, under Keying & Mattes, has two optional Mask inputs, `a` and `b`, and
one Mask out, `mask`, covering the composition canvas. Its Combine control (`mode`, a constant
dropdown) is Add (0, the default), Subtract (1), Intersect (2) or Difference (3), with ADR 0028's
layer-mask arithmetic for coverage `a` and `b`:

| Mode | Coverage |
| --- | --- |
| Add | `a + b - a b` |
| Subtract | `a (1 - b)` |
| Intersect | `a b` |
| Difference | `a + b - 2 a b` |

An unconnected input is zero coverage. `a` is placed on the canvas (white times `a`), then combined:
Add is Image Mix of it towards white by `b`; Subtract and Intersect are Multiply Alpha by `b`
(inverted for Subtract); Difference is Image Mix of it towards its own complement by `b`.

### Image to Mask

`bloom.image-to-mask`, schema 1, under Keying & Mattes, has one Image in, `input`, and one Mask out,
`mask`, covering the composition canvas. Its Matte channel control (`channel`) is the matte
selector Set Alpha and Multiply Alpha use: Alpha (0, the default), Red, Green, Blue, Luma, and the
same five inverted (5 to 9). The Mask is the canvas multiplied by the selected value (Multiply
Alpha), so a colour channel or luma reads the premultiplied stored value.

### Effect mask

Every image effect in the Grade, Color and Filters categories (`NodeCategory`, not the Add menu's
groups) whose leading input is `input` and
whose only output is an Image `image` takes an optional `mask` input after its own inputs, and a
Mask channel control (`maskChannel`, the same ten-value matte selector, default Alpha) after its
own controls. Glow keeps its own `mask` input and so takes none; compositing recipes, transforms
and combines take none. The input accepts a Mask, and an Image read through the selector; a Mask
is always read as its coverage, so only the selector's inverted half applies to one. With nothing
connected the effect lowers exactly as before.

With mask coverage `m` the output is the effect's result where the mask covers it and the effect's
own input elsewhere, `lerp(input, effect, m)`. A grade or colour-management effect keeps its
input's data window, so it lowers to one Image Mix of the input towards the effect's result by the
mask (after a Multiply Alpha of white by the mask when the selector is not plain Alpha). A filter
may grow the window (a blur's halo), so there the effect's result is the base: Image Mix of the
result towards the input by white-over-the-result times `1 - m` (Multiply Alpha with the selector's
invert flipped), which keeps the halo.

### Node mattes on layers

A layer mask may take its shape from a node's Mask output instead of a path (`LayerMask.matte`, an
output port reference; document 1.42). It keeps its name, mode, Inverted and Opacity; Path, Feather
and Expansion do not apply. Its coverage is the Mask, inverted on request, times the opacity, in
the layer's content space at the layer's time (the Mask's pixels as they lie on the canvas, with no
layer transform applied to it). Masks without a matte lower to the single `LayerMaskKernel` as
before. When any mask has a matte, every mask's coverage is its own Mask over the content's data
window -- a path mask the kernel over opaque white (a leading run of path masks stays one kernel),
a matte mask Multiply Alpha of white by its node's Mask -- accumulated in list order by the same
modes and start rule as the kernel, and the content is multiplied by the result. An Intersect
matte mask, and a Subtract one at a constant full opacity, multiplies the accumulation by its Mask
directly (one Multiply Alpha, the invert and opacity in its selector and factor).

A track matte may read a node's Mask instead of another layer (`TrackMatte.node`, with no source
layer). Channel and Inverted apply as for a layer matte; the Mask is in composition space at the
composition's time, and the target is composited through the same Multiply Alpha a layer matte
uses. The matte node must produce a Mask and must not depend on the layer it shapes (a cycle is
refused). Removing the node removes the masks it shapes and clears the track mattes it feeds, in
the same undoable edit.

### Evidence

`bloom.runtime.matte-workflow` checks every Mask Combine mode and an unconnected input, Image to
Mask's channels, an effect limited by a Mask and by an Image channel (inverted and muted), matte
masks among path masks (every mode, invert, opacity, toggling the matte, removing its node), a
masked filter, and node track mattes (Alpha, inverted Luma, refusals, removal) on the CPU
reference. The Cryptomatte test drives a Cryptomatte selection as a matte mask and as a node track
matte against its reference coverage, and re-masks the layer when the selection changes. The GPU
coverage contract prepares each route's production plan, and its native acceptance runs the two
nodes on the device against the CPU reference (exact alpha) cold, then warm with no dispatch. The
node cost bench (benchmarks.md, "Node cost") times every route cold, warm and per frame at 1080p
and 2160p against a Grade and the closest existing nodes, and the `cryptomatte-mattes-2k` scenario
of the real-world suite plays, fills and exports a Cryptomatte matte mask, a masked Grade and a
node track matte on the bench EXR.

## Tile & Mirror

One node, `bloom.tile-mirror`, under **Transform** (#511): the picture repeated, panned with wrap,
reflected about a line, or folded into a kaleidoscope. It is the same inverse mapping as Corner Pin,
ST Map and Displace (below): for every output pixel a position of the input is computed and read, so
each mode lowers to one field raster (`fields.md`), the CPU reference and the GPU run the same program,
and nothing in it is a GPU coverage exception. The instructions are existing ones (Coordinate,
Constant, Swizzle, Math, Transform, Homography, Image); the node adds no kernel.

A **Mode** (a constant) picks what is read. Only the values of the mode are read, shown and compiled; a
keyed value of another mode neither varies the picture with time nor invalidates its cache
(`isParameterInactive`). Field positions are pixels from the composition frame's centre with y up; the
node's points are pixels from that centre with y down, and its angles turn clockwise.

| Mode | Position read | Values |
| --- | --- | --- |
| Tile | The pixel, counted in tiles, is folded into one cell and read back at the tile centre. | Tile Center (point), Tile Width and Height (percent of the composition frame, 100 by default; Link Tile Size gives both the width), Horizontal and Vertical Phase (degrees), Mirror Edges, Clamp to Output with Output Width and Height (percent of the frame). |
| Wrap Offset | The pixel less the shift, the edge rule Repeat about the input's data window. | Offset (point, pixels, y down). |
| Mirror | The pixel reflected about a line, on one side only. | Line Point, Line Angle (degrees clockwise from screen right, 90 is vertical), Reflect. |
| Kaleidoscope | The pixel turned, divided by the size and folded into a wedge. | Center, Segments (1 to 64), Rotation (degrees clockwise), Size (percent). |

**Filter** (Bilinear, Nearest, Bicubic) is shared by every mode. **Edges** (Transparent, Extend, Repeat,
Mirror) is read by every mode but Wrap Offset, whose rule is Repeat: it names what shows where a read
leaves the input.

**Tile cell rule.** With tile size `(tw, th)` pixels (`tw = Tile Width x frame width / 100`), phase
`(hp, vp)` and centre `C` (y up), the pixel at `p` has `u = (p.x / tw - hp / 360, p.y / th + vp / 360)`
and reads `C + (f(u.x) tw, f(u.y) th)`, where `f(t) = t + 1/2 - floor(t + 1/2) - 1/2` is the position in
the cell, in [-1/2, 1/2). The cell that holds the frame's centre is the tile at `C`, whole; the pattern
repeats from there. A phase of 360 degrees is one whole tile, so an animated phase loops. A positive
horizontal phase moves the tiles right and a positive vertical phase moves them down. With **Mirror
Edges** the position in the cell is the triangle wave `g(m)` of `m = t + 1/2` (`m - 2 floor(m / 2)`,
reflected above 1) less 1/2, so each neighbouring tile is flipped. With **Clamp to Output** a pixel
farther from the centre than half the output size (percent of the frame) is transparent; with it off the
tiles fill the whole canvas and the output size is not read. The canvas is the composition frame, so the
tiles can fill the composition whatever the size of the input. The input's data window is laid with its
centre at the frame's centre (the translation by the window's centre, Homography choice 2 in `fields.md`),
and the cell, the Tile Center and the phase are placed in the frame about it: for a picture that fills the
frame the translation is zero, and for a shape or text layer, whose window is its own small box, a 40%
tile puts a copy of the box in every cell and fills the frame. Tile sizes are in full-resolution
composition pixels: a reduced render scale draws the same pattern at fewer pixels, and the cell edges
lie on pixel edges at any scale only where the tile size in pixels is a whole number there.

**Wrap Offset.** The shift is in full-resolution composition pixels. The output keeps the input's data
window, and the picture wraps about that window (the sampler's Repeat edge wraps the filter's taps too),
so a whole-pixel shift is the exact rotation of the pixels with no seam, for every filter. At a reduced
render scale an odd shift is a fraction of a pixel and the filter blends across it.

**Mirror.** The line goes through the point at the angle. **Reflect** is **Left or Below** (the default)
or **Right or Above**: the pixels on that side are kept as the input has them and the pixels on the other
side show their reflection. At 90 degrees the line is vertical and the sides are left and right; at 0
it is horizontal and they are below and above. (The kept side is the one the line's direction turned a
quarter turn clockwise points to; 180 degrees swaps the sides.) The data window is the input's.

**Kaleidoscope.** The pixel less the centre is turned counter-clockwise by the rotation (so the pattern
turns clockwise), divided by the size (a larger size shows less of the picture, larger), folded into the
wedge of `1 / segments` turn that starts at screen right (the second half of each wedge is the mirror
image of the first), and read about the centre. The result has the symmetry of a dihedral group of the
segment count. The canvas is the composition frame, as for Tile: the centre and the wedge are placed in
the frame, and the input's window is laid with its centre at the frame's centre, so the kaleidoscope of
a small shape layer fills the frame. For a picture that fills the frame the result is as if the window
were the frame.

The picture's colour is not changed: values above one, negatives and alpha keep their range (the read
is the filtered premultiplied texel as stored). A position beyond a billion pixels reads transparent.
Because a position can be anywhere, a region of interest reads the whole input. A muted node passes its
input through.

Cost, from the code: one raster pass; Tile and Kaleidoscope compute about a dozen register operations
per pixel before the read, Bicubic reads sixteen texels per pixel, Bilinear four, Nearest one. No timing
is recorded; see [Benchmarks](../development/benchmarks.md).

Tests: `snapshot_compiler_tile_mirror_tests` (lowering per mode, the coverage fixture's program is the
lowering's, activity and time dependence, and the pixels against independent binary64 readings of the
cell rule, the wrap, the reflection and the fold, plus symmetry, region of interest, render scale,
budget and cancellation), the Required `node.bloom.tile-mirror` GPU coverage fixture and the native
CPU/GPU parity cases.

## Procedural Warp

One node, `bloom.procedural-warp`, under **Distort** (#510): the procedural distortions a compositor
coming from a layer-based compositor reaches for, one **Mode** (a constant) at a time. It is the same inverse
mapping as Corner Pin, ST Map, Displace and Tile & Mirror: for every output pixel a position of the
input is computed and read, so every mode lowers to one field raster of one instruction,
`ProceduralWarp` (`fields.md`, "Procedural warp", has the arithmetic of each mode). The CPU reference
and the GPU run the same program, and nothing in it is a GPU coverage exception.

Only the values of the mode are read, shown and compiled; a keyed value of another mode neither varies
the picture with time nor invalidates its cache (`isParameterInactive`, and the lowering puts the
default of every other value into the program as a constant). **Center** is in pixels from the
composition frame's centre with y down; lengths are full-resolution composition pixels; angles are
degrees.

| Mode | What it does | Values |
| --- | --- | --- |
| Twirl | Turns the picture about the centre, most at the centre and none at the radius. A positive angle turns it clockwise. | Center, Radius (300 px), Falloff (100%), Angle (90 degrees, a dial). |
| Bulge / Pinch | Swells (positive) or pinches (negative) the picture inside the radius. | Center, Radius, Falloff, Amount (-100 to 100%, 50%). |
| Spherize / Lens | Looks through a glass sphere cap: the picture is magnified at the centre and compressed towards the rim, as refraction gives it. A negative amount is the inverse (the picture drawn onto the cap). | Center, Radius, Amount, Refraction Index (1 to 4, 1.5). |
| Ripple | Concentric rings travelling outward from the centre. | Center, Radius, Falloff, Wavelength (40 px), Amplitude (10 px), Phase (degrees), Speed (cycles a second). |
| Wave | Parallel waves across the whole frame, sideways to their travel. | Wave (Sine, Triangle, Square, Saw), Wavelength, Amplitude, Direction (90 degrees, clockwise from up, a dial), Phase, Speed, Pin Edges (None, All Edges, Left, Right, Top, Bottom, Left and Right, Top and Bottom). |
| Polar Coordinates | Wraps the frame into a disc about its centre (Rectangular to Polar), or unwraps a disc into the frame (Polar to Rectangular). | Conversion, Interpolation (100%: 0 is the picture, 100 the full conversion). |
| Magnify | A loupe: a circle or square in which the picture is enlarged about the centre. | Center, Shape, Radius, Magnification (200%), Grow with Magnification (Nothing, Size, Size and Feather), Feather (px, inside the edge). |

**Filter** (Bilinear, Nearest, Bicubic) and **Edges** (Transparent, Extend, Repeat, Mirror) are shared by
every mode: the filter is how a read between texels is made, the edge rule what shows where a read
leaves the input.

**Falloff.** Twirl, Bulge / Pinch and Ripple fade their strength towards the radius by the same weight,
`smooth01((1 - r / radius) / falloff)`: at 100% the strength fades over the whole radius, at 0% it is
full right up to the radius (a hard edge). Spherize and Magnify have their own edge (the cap's rim, the
feather); outside the radius every radial mode leaves the picture as it is.

**Motion.** Ripple and Wave move with **Phase** (degrees, one cycle is 360) and **Speed** (cycles a
second, 0 by default): the phase of a frame is `phase / 360 + speed x seconds`, resolved on the host from
the composition time. With a speed of 0 and nothing keyed, the node does not depend on time and keeps
one cache entry across frames.

**Wave and Polar Coordinates use the composition frame.** A Wave's position along its travel is measured
from the frame's centre and Pin Edges holds the chosen frame edges still, easing the waves in over one
wavelength from each pinned edge. Polar Coordinates converts about the frame's centre with a polar
radius of half the shorter side, angle 0 up and increasing clockwise, the frame's left edge at angle 0
and its top row at the centre: Rectangular to Polar maps the frame's columns to angles and its rows to
radii, so a horizontal line becomes a circle.

**Magnify** divides the offset from the centre by the magnification inside the shape. **Grow with
Magnification** scales the radius (Size) or the radius and feather (Size and Feather) by the
magnification, so the loupe covers the same share of the picture it enlarges; Nothing keeps the radius
as set. The feather softens the edge inside the shape by blending the magnified read over the plain
picture (two reads of the input), never by smearing it.

The picture's colour is not changed: values above one, negatives and alpha keep their range (the read is
the filtered premultiplied texel as stored). The output keeps the input's data window; because a read
can be anywhere, a region of interest reads the whole input. A muted node, a radial mode with a
constant radius of 0 and a mode whose strength (angle, amount, amplitude, interpolation, or a
magnification of 100%) is a constant zero pass the input through. A position too far to represent reads
transparent.

Cost, from the code: one raster pass of one instruction (two image reads for Magnify); Spherize's inverse
(a negative amount) evaluates the cap map twenty times per pixel inside the radius; Bicubic reads sixteen
texels per read, Bilinear four, Nearest one. No timing is recorded; see
[Benchmarks](../development/benchmarks.md).

Tests: `procedural_warp_field_tests` (each mode's instruction against the independent binary64 reference
in `src/core/tests/procedural_warp_reference.hpp`: identity at zero strength, falloff, signs and
directions, the wave shapes and pinning, both polar conversions, the magnify shapes and feather, far
positions), `snapshot_compiler_procedural_warp_tests` (lowering per mode, the coverage fixture's program,
activity and time dependence, pass-through cases, the pixels of every mode against the reference, region
of interest, render scale, edge rule, budget, cancellation and cache identity), the Required
`node.bloom.procedural-warp` and `feature.field.op.procedural-warp` GPU coverage fixtures and the native
CPU/GPU parity cases (every mode through every filter). The user guide is
[Procedural Warp](../user-guide/procedural-warp.md).

## Corner Pin, ST Map and Displace

Stabilize (#513) is a fourth user of the same inverse mapping and is specified in
`point-tracking.md`.

Three nodes under **Distort** (#348) that move a picture by reading it somewhere else. Each is an
inverse mapping: for every output pixel a position of the input is chosen and read. They differ only in
where the position comes from. All three lower to one field raster (`fields.md`), so the CPU reference
and the GPU run the same program, and none is a GPU coverage exception.

| Node | Position read |
| --- | --- |
| Corner Pin (`bloom.corner-pin`) | The input's data window pinned to a quad of four corners, with perspective. Each corner is an animatable offset in pixels from where the input's own corner is (y down). The default is all zero, the identity. |
| ST Map (`bloom.st-map`) | Absolute: two chosen channels of the `map` input hold the position as a fraction of the composition frame (0, 0 bottom left, 1, 1 top right). This is the map Lens Distortion writes. |
| Displace (`bloom.displace`) | Relative: the pixel's own position plus `(value - midpoint) * amount` pixels per axis, from the chosen X and Y channels of the `map` input (None leaves an axis alone). |

Common rules:

- **Raw maps.** The map is read as the data it is: unclamped (values below zero and above one keep
  their meaning), with no colour conversion, and never through an input colour space. The texel is read as
  stored, so a map with partial alpha is not unpremultiplied.
- **Filter** is Bilinear, Nearest (exactly one texel, for ID and data pictures) or Bicubic (Catmull-Rom).
  **Edges** (ST Map, Displace) is Transparent, Extend, Repeat or Mirror. Corner Pin reads outside its
  window as transparent.
- The picture's colour is not changed: values above one, negatives and alpha keep their range.
- Without a connected map, ST Map and Displace pass the picture through unchanged.
- Corner Pin with a degenerate quad (collinear corners, or a fold through the horizon) shows nothing where
  the map is undefined rather than a smear.
- A huge map value cannot overflow: a position beyond a billion pixels reads transparent.
- Because the position can point anywhere, a region of interest reads the whole input.

Cost, from the code: one raster pass; Bicubic reads sixteen texels per pixel, Bilinear four, Nearest one.
No timing is recorded; see [Benchmarks](../development/benchmarks.md#corner-pin-st-map-and-displace-348).

Tests: `field_program_tests` (the Homography against an independent forward quad map, identity,
degenerate quads, window mode, far-position guard), `snapshot_compiler_warp_tests` (lowering and pixels,
tied to Lens Distortion's ST map), the three Required GPU coverage fixtures and the native CPU/GPU
parity cases.
