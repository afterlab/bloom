# Image Filters

Status: implemented; grouped native and cross-platform qualification pending

Updated: 2026-11-03

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
