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
