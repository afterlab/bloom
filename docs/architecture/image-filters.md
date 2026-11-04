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
Image-out grades listed under Grade in the Add menu. None of them performs a colour transform or
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

`bloom.grade`, schema 1 (#494), is an Image-in, Image-out primary grade listed under Grade, in the
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
channels and build an image from channels. Both are listed under Compositing in the Add menu,
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

## Drop Shadow, Glow and Light Wrap

`bloom.drop-shadow`, `bloom.glow` and `bloom.light-wrap`, schema 1 (#339), are compositing
recipes listed under Compositing in the Add menu. Each has a required Image input, `input`, and an
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

Glow parameters and defaults: `threshold` 1.0 (so only over-white HDR values glow by default),
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
Levels scales its alpha by `amount` (a separate gain rather than Multiply Alpha's factor, so the
alpha is one rounding on the CPU and the device alike); and a Merge entry with the explicit Atop
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
background, full frame and ROI); there is no exception. Glow's device cases key an HDR,
partial-alpha solid: Luma Key's coverage is a function of colour, and a generated gradient's colour
is within 2e-6 of the CPU reference on a device rather than bit-exact, so only a solid source can
meet the exact-alpha gate.

Tests: the direct recipe test compares each chain and its variants (Shadow Only; a hard one-octave
Glow Only; a keyed, masked Glow; Wrap Only) with an independent double-precision reference (direct
2D Gaussians, the straight luma ramp, the octave sum and the Porter-Duff composites) at 1e-4, and
covers warm reuse, invalidation, ROI, animation, cancellation and budget refusal. Compiler tests
cover lowering, folding, octave radii and weights, the Merge modes, animated values and mute;
registry, model, command, Properties and archive tests cover the definitions, wiring, switches,
refusals, undo/redo and reopen. The scene-preparation test checks the Add, Atop and In blend
commands, and the native image-effect harness runs all three recipes against the CPU oracle at the
2e-6/exact-alpha gate with warm zero-dispatch and changed-value runs.
