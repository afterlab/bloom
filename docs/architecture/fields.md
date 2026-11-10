# Fields

Status: built (#525). This document is the specification of the Fields node family, its data
type, the field program, every field operation and the raster and sample boundaries. The code that
owns each part names this document.

## Data type

A field is an immutable, typed, resolution-independent function of a **field position**:
composition pixels from the composition's centre, +x right, +y up. Three socket kinds carry fields:
**ScalarField**, **Vector2Field** and **ColorField** (`document::SocketValueKind`). A field is not
an Image or a Mask: it has no data window, it never converts implicitly, and a field socket connects
only to its own kind or to a Fields input that reads any field (`anyField` in
`document/field_nodes.hpp`). Field sockets are drawn as diamonds in their own inks
(`ux/visual-language.md`).

There are exactly two explicit boundaries out of a field:

- a **raster** turns a field into an Image or a Mask over an explicit extent: the Rasterize Field
  node, or a generator's own `image` and `mask` outputs (the same raster, fused, so Noise feeds an
  image input with no extra node); Colorize's `input` also takes a field (issue 3.4), which is the
  same raw raster over the composition (a scalar field as grey), declared on that one input
  (`acceptsFieldKinds`), never an implicit conversion on any other image input;
- **Field Sample** reads a field at one position as Scalar, Vector 2 and Color values, which drive
  any parameter like every other value node.

## The field program

A field never exists as pixels between nodes. The compiler walks the field edges upstream from a
raster or a Field Sample and compiles every field it reads into one **field program**
(`runtime::FieldProgram`, `src/runtime/snapshot_compiler_fields.ipp`):

- at most 64 instructions over 16 vec4 registers, at most 4 image slots and 16 sub-samples;
- each node is compiled at a coordinate register; a Field Transform or a Field Derivative compiles
  its input at a moved coordinate, and a node shared by two consumers at one coordinate is compiled
  once (memoized per node, port and coordinate);
- virtual registers are packed into the sixteen physical ones by last use; a program past either
  limit is refused with a named compile diagnostic, never truncated;
- a muted field node passes its first field input through (or is zero); a muted raster draws
  nothing.

Each frame the program's operands (Scalar, Vector 2, curve and ramp parameters, constant, keyed or
driven) are sampled and encoded with the clock time into **one binary32 table**
(`runtime/field_raster.hpp` has every operation's operand order; `core/field_program.hpp` the
table layout). Time is consumed while encoding (a loop's phase, a pattern's speed), so a program
that does not read the clock has one table, one cache identity and one cache entry across frames.

### One kernel, two hosts

`src/core/field_kernel.glsl` is the only implementation of every operation. It is compiled twice:
as C++ by `src/core/field_program.cpp` (the CPU reference, built without contraction or fast math)
and as GLSL 4.60 inside the GPU program `runtime::detail::fieldRasterProgramText()`. Both hosts
interpret the same table. Every intermediate is a `precise` binary32 value; every transcendental
(exp2, log2, sin, cos, atan2, pow) is Bloom's own polynomial, because a device's built-ins are only
specified to a few ulps; lattice hashing is integer only (SplitMix64-keyed seeds,
`core::primitives::hashWord`), and the seed is hashed with the lattice cell, never added to a
coordinate. A device's `sqrt` is also only specified to a few ulps, so `bfk_sqrt` corrects it: from
the device's root it steps (at most three times) to whichever neighbouring binary32 value squares
closest to the argument, each residual taken with one `fma`. For every binary32 argument the
closest square is the correctly rounded root, which the CPU's `sqrt` already gives, so every host
evaluates the same binary32 operations. The coverage contract's native acceptance holds a Fields
node to that (2e-6 on colour, exact alpha).

### Rasters

A raster is `runtime::FieldRasterKernel` on a canvas, lowered as a colour-grade kind
(`ColorGradeKind::FieldRaster`):

- the canvas is a transparent composition-size Solid, the `extentImage`'s data window (Extent:
  Input Image), or an Explicit rectangle cut from the composition canvas by exact Crop and Pad
  margins; the raster replaces every pixel of the canvas's data window;
- pixel (i, j)'s sub-sample (i + ox, j + oy) is at field position ((i + ox) / h − W / 2,
  H / 2 − (j + oy) / v) for composition size W × H and evaluation scale (h, v), so a proxy
  evaluates the same field;
- sub-samples are 1, 4 or 16 on a regular grid, averaged;
- output modes: Raw Data (a scalar in RGB with alpha 1, a vector in R and G), Mask (the scalar
  clamped to [0, 1] in every channel) and Colour (a premultiplied colour field, or a scalar through
  the ramp);
- octaves above half a cycle per pixel fade out over the sampling footprint, so a proxy keeps the
  look of full resolution.

On the CPU the raster runs in row bands under the request's pixel budget with cancellation per row
(`applyFieldRaster`). On the GPU it is one dispatch of the fixed program
(`gpu_scene_field_emission.ipp`): without images over the canvas window; with images over an
**atlas** that places each image slot beside the target by exact integer translations composited
over a transparent base, after which the target is cut back out. The program reads a slot at its
atlas position with the CPU's own bounds, filter and edge rules. The table, and so the prepared
command, carries the geometry; a keyed parameter changes only data, never the program.

### Test Card

The Test Card source (`user-guide/test-card.md`, #587) lowers to a field raster of one instruction,
`core::field::FieldOp::TestCard` (a Required GPU coverage feature as every operation is), over the coordinate,
followed by one color space transform into the working space (none for a working-space Patch). The
instruction draws all sixteen cards from one kernel source shared by the CPU reference and the GPU: it
receives the card's pixel size and center as data and computes everything from the pixel's position in the
card's own frame, so no raster is stored and nothing has a fixed resolution. A card's size is the composition's
unless Custom; the canvas under the raster is cut or padded about the composition's center with integer
margins. The raster result is the premultiplied signal in the card's own color space (signal levels outside 0
to 1 are kept, which the PLUGE and HDR cards need). Time (the stepping Patch, the moving Zone Plate and Moving
Bar, the Countdown) arrives as host-resolved scalars, so a still card is not time dependent. The Zone Plate and
the Siemens Star run 16 sub-samples a pixel, the other cards 4.

Evidence: `field_program_tests.cpp` (`testTestCard`) holds all sixteen cards at kernel level, the bars,
Patch, Circle, Zone Plate, Siemens Star and Countdown among them. Through the production compile and the CPU
reference `snapshot_compiler_test_card_tests.ipp` holds Patch, Circle, Custom size, the padded bars and the
PLUGE steps (`testTestCardPixels` and its neighbours) and, in `testTestCardEveryCardPixels`, the Color Chart
(all 24 patch centres), Gray Ramp (the ramp's pixels and the nine PLUGE segments), Grid (line and ground
pixels at the spacing and width asked, yellow centre lines, square cells in a wide, a portrait and a square
frame), Safe Areas (93% and 90% outlines and the four crop guides over the whole frame), UV Grid (cell colours,
cell lines, the written column and row), Moving Bar (the bar at three times, both directions, and on a proxy)
and Alpha Ramp (alpha by column, straight colour, premultiplied pixels, and on a proxy), each against the
levels and fractions the user guide gives, taken through the colour library's Rec.709 transform.

### Four-Color Gradient

The Four-Color Gradient source (`user-guide/four-color-gradient.md`, #524) lowers to a field raster built only
from the existing field vocabulary, so it adds no instruction and no shader. Over a transparent
composition-sized canvas, each of the four points becomes a Transform translation of the coordinate (the
field position minus the point, pixels from the center, +y up), its squared length (a dot product, so no
square root, which a device may round differently from the CPU) in units of the squared frame diagonal and
padded by a quarter pixel squared so a point's own pixel stays finite, divided by the nearest point's (the minimum over the four) and raised to the power `-blend / 2`, so the
nearest point weighs exactly 1 and the weight sum is at least 1: the unnormalized power underflows binary32
(and flushes to zero on a device) when every point is far from the pixel, a small frame, points near the
domain's limit or a high blend, which left 0 / 0. A new node's points are written at the composition's
corners by the Add Node command, resolved from the composition's size at creation like a Solid's size.
Each weight multiplies its color (a four-lane Constant of the color parameter, alpha included); the straight
color is the weighted sum over the weight sum, premultiplied once by the swizzle `(a, a, a, 1)`. The
unit-diagonal measure keeps a weight a normal binary32 number for the document's point and blend domains.
Points and colors are animatable operands, so moving or keying them changes data, never the program. The
coverage fixture `node.bloom.four-color-gradient` holds the CPU reference and the device to the same program
at a soft, the default and a sharp blend, full frame and region of interest.

### Lens Warp

Lens Distortion (`image-filters.md`; Chromatic Aberration lowers to the same instruction) lowers to field rasters, and adds one instruction and two small
extensions to the vocabulary below (`core::field::FieldOp::LensWarp`, a Required GPU coverage feature as
every operation is). **Lens Warp** takes a coordinate and returns the position to read: the Brown-Conrady
model about a centre in normalised units (the scale a length in field pixels), forward, or inverted by
twelve Newton steps over its Jacobian. The **Image** instruction gains selector 3, the filtered texel as
stored (premultiplied), and a raster gains result kind 3, **Premultiplied**, which keeps a premultiplied
register whole, colour at zero alpha included (the straight colour of kind 2 cannot carry it). A raster
that reads an image through a position it computes is evaluated over the whole image in a region of
interest, since the position can point anywhere. These change no existing operation, so
`kFieldProgramSemanticsVersion` stays.

### Homography

Corner Pin, ST Map and Displace (`image-filters.md`) share one inverse mapping: each output pixel picks
the position of an input to read. ST Map and Displace build that position from existing instructions.
Corner Pin adds one, `core::field::FieldOp::Homography` (a Required GPU coverage feature), a projective
map `(x, y) -> (m0 x + m1 y + m2, m3 x + m4 y + m5) / (m6 x + m7 y + 1)` about a centre. Its matrix is
solved on the host for the frame from the quad (`homographyFromQuads`, a centroid-normalised direct
linear transform over an eight by eight system) and written into the table, so the kernel only
evaluates it. In window mode the geometry pass places the quad against the data window of the slot's
image, so the identity default holds for any input size on the CPU and in the GPU atlas. A map that is
singular, or a position behind the horizon (w at or below a small epsilon), returns a position far
outside any image, which reads as transparent. The sampler returns transparent for any position beyond
a billion pixels, so a huge map value cannot overflow the sample arithmetic. These change no existing
operation, so `kFieldProgramSemanticsVersion` stays.

Homography has a second mode, **translation** (choices `[1, direction]`, vectors `[held point, tracked
position]`), used by Stabilize (`point-tracking.md`): the similarity map with the offset between the
two points (no turn, no scale, no window), `direction` 0 holding the picture still and 1 carrying it
along. It is encoded by the same `similarityHomography` and `encodeHomography` as the quad mode.

A third mode, **window centre** (choice `[2]`, the input slot), is the translation `q = p + c` by the
centre `c` of the slot's data window, solved when the window is placed like the quad mode. Tile & Mirror
ends its Tile and Kaleidoscope programs with it, so a window of any size and place (a shape or text
layer's own small box) is read as if its centre sat at the field origin; for a window that fills the
frame `c` is zero. It is the same table block, with the flag in the window block's third lane, and the
kernel is unchanged.

### Procedural warp

Procedural Warp (`image-filters.md`) lowers to a field raster and adds one instruction to the
vocabulary (`core::field::FieldOp::ProceduralWarp`, op 25, a Required GPU coverage feature as every
operation is). **Procedural Warp** takes a coordinate `p` (field pixels, y up) and returns
`[qx, qy, weight, 0]`: the position `q` to read and a weight that is 1 for every mode but Magnify.
Every mode writes `q` as `p` plus a move, so a mode whose strength is zero returns `p` exactly. The
block is four vec4, `[mode, centre x, centre y, radius] [1 / radius, a0, a1, a2] [a3 .. a6]
[a7 .. a10]`; the host (`encodeProceduralWarp`) resolves every angle to turns, every reciprocal, and
the per-frame phase `frac(phase / 360 + speed x seconds)` in binary64, so the kernel holds no clock
and divides by no operand that can be zero. The stored mode values are 0 Twirl, 1 Bulge / Pinch,
2 Spherize, 3 Ripple, 4 Wave, 5 Polar Coordinates, 6 Magnify, 7 Warp, 8 Bend, 9 Split, 10 Turbulent,
11 Drizzle; a new mode is appended after Drizzle (`kProceduralWarpModeCount`, 12).

The radial modes (Twirl, Bulge / Pinch, Spherize, Ripple, Magnify) work on `d = p - centre`,
`r = |d|` and `t = r / radius`. They leave `p` alone at and beyond the radius (tested on the larger
axis first, so a far position is never squared) and when the radius is not positive. The falloff
weight Twirl, Bulge / Pinch and Ripple share is `g = smooth01((1 - t) / falloff)`, `smooth01` the
cubic smoothstep on [0, 1] (1 with a falloff of 0).

| Mode | Move |
| --- | --- |
| Twirl | `d` turned counter-clockwise (y up) by `angle x g`, so a positive angle turns the picture clockwise on screen. |
| Bulge / Pinch | `d` scaled by `sin(t x 90 degrees)^(amount x g)`: a positive amount reads nearer the centre (a bulge), a negative one further out (a pinch). |
| Spherize | A refracting sphere cap of index `n`: with `h = sqrt(1 - t^2)` (taken as `(1 - t)(1 + t)`), `theta = asin t` and `theta' = asin(t / n)`, `m(t) = t - h tan(theta - theta')` is the radius light through the cap comes from. A positive amount reads at `t - amount (t - m(t))`; a negative one at the inverse of that map at `|amount|`, found by 20 halvings of [0, 1]. |
| Ripple | `d` moved along itself by `amplitude x sin(r / wavelength - phase)` (the argument in turns) `x g`, eased in by `smooth01(2 r / wavelength)` so the centre does not tear. |
| Wave | `p` read back along the normal of the travel direction (clockwise from up) by `amplitude x w(p . travel / wavelength - phase)`, `w` the Sine, Triangle, Square or Saw wave in [-1, 1], 0 at 0 and rising; each pinned frame edge (a mask: 1 left, 2 right, 4 top, 8 bottom) multiplies the move by `smooth01(distance to the edge / wavelength)`. |
| Polar Coordinates | About the frame's centre, angles clockwise from up, the frame's left edge at angle 0 and its top row at the pole, the polar radius half the shorter side: Rectangular to Polar reads the frame position whose column is the pixel's angle and whose row is its radius; Polar to Rectangular the inverse. The move to the converted position is scaled by the interpolation. |
| Magnify | Inside the circle or square of the radius (the larger axis for a square), `d` divided by the magnification, with the weight `smooth01((radius - distance) / feather)` (1 with no feather); 0 outside. |
| Magnify, pull | A second point `c2` (`pullPoint`) drags the loupe: inside the shape the read position is moved by `-pull x (c2 - c) x smooth01((size - distance) / size)` as well, `size` the grown radius. |
| Ripple, pulse | With a pulse radius or speed the front `f = pulseRadius + pulseSpeed x seconds` (the time resolved on the host) limits the rings: each ring's amplitude is scaled by `smooth01((f - r) / wavelength)`, so only the rings behind the front move. |
| Warp | In frame coordinates `u = x / (w / 2)`, `v = y / (h / 2)`, the horizontal and vertical distortion are a perspective, `u1 = u / (1 + 0.5 h cv)`, `v1 = v / (1 + 0.5 vd cu)`, then the style of the 13 (Arc, Arc Lower, Arc Upper, Arch, Bulge, Flag, Wave, Fish, Rise, Fisheye, Inflate, Squeeze, Twist) offsets `(u1, v1)` to `(u2, v2)` by the bend; the move is `((u2 - u) w / 2, (v2 - v) h / 2)`, so a zero bend with no distortion is the identity. |
| Bend | `w = (distance along the direction from the centre) / (height / 2)`; the move along the direction's normal is `bend x (1 - w^2)^2` for `|w| < 1` and 0 outside. |
| Split | With `sd` the signed distance to the line (its normal to the left), `gapL = splitDistance / 2` and `gapR = oppositeDistance / 2` (`gapR = gapL` unless Asymmetric is on, so the default split is symmetric): a pixel with `sd > gapL` reads `gapL` nearer the line, one with `sd < -gapR` reads `gapR` nearer it on the other side, and the gap `-gapR < sd < gapL` has the weight 0 (a Multiply by the weight swizzle in the program, transparent). With Mirror the side `sd < d` reads the reflection about the line `sd = d`, the other side being kept, and Asymmetric is ignored. The encoder writes `gapR` in the instruction's sixth operand slot (`a5`). |
| Turbulent | `q = p + displacement x w x (n0, n1)`, `n0, n1` the field Noise operation's streams 0 and 1 (Perlin fractal Brownian, bipolar, `complexity` octaves, `noiseSize` and `noiseOffset` in pixels, evolution in turns) and `w` the product of `smooth01(distance to each pinned edge / ease length)`, the ease length the larger noise size. |
| Drizzle | Drop `i` of `drops` (at most 64) is placed by `pcg4d` of `(i, 0..2, 0, seed)` as unit values `u0..u2`: its centre `((2 u0 - 1) w / 2, (2 u1 - 1) h / 2)` and age `fract(phase + u2)`. Its ring is at `r = age x radius` and moves the pixels inside it outward by `amplitude (1 - age) smooth01((front - r) / wavelength) sin(-(front - r) / wavelength turns)`; the drops' moves add. |

Magnify's raster reads the image twice, at `p` and at `q`, and blends from the plain read to the
magnified one by the weight (Math Lerp), so the feather is a soft edge, not a smear. A position
beyond 1e15 on an axis reads 1e20 in Polar Coordinates, which no image reaches. The independent
binary64 reference is `src/core/tests/procedural_warp_reference.hpp`. The op changes no existing
operation, so `kFieldProgramSemanticsVersion` stays.

### Rays

Light Rays (`image-filters.md`) lowers to a field raster and adds one instruction to the vocabulary
(`core::field::FieldOp::Rays`, a Required GPU coverage feature as every operation is). **Rays** takes a
coordinate `p` and gathers one image slot along the segment from `p` towards a center, covering a
`length` fraction of the way: `n = min(floor(reach / 2) + 1, 96)` samples, where `reach` is the segment's
length in field pixels, so a short ray takes few samples; sample `i` lies at the fraction
`t = (i + 1/2) / n`, is the bilinear premultiplied texel's Rec. 709 luma (source 0) or alpha (source 1),
transparent outside the image, and is weighed `(1 - t)^falloff`. The result is the weighted mean times
the boost, `[m, m, m, min(m, 1)]`. A pixel at the center, a zero length and a zero boost give zero. A raster
that gathers an image is evaluated over the whole image in a region of interest, as Lens Warp's is. The
op changes no existing operation, so `kFieldProgramSemanticsVersion` stays.

### Image slots

An Image Field reads an image through a slot; a mask Shape Distance reads the **exact signed
distance** of its mask (`runtime::MaskDistanceKernel`). The distance is Felzenszwalb and
Huttenlocher's separable squared distance transform ("Distance Transforms of Sampled Functions",
Theory of Computing 8, 2012) on the CPU; on the GPU two fixed passes compute the same minima by a
bounded search within the reach (columns, then rows), so both paths produce identical exact
distances. The output window grows by the reach (the falloff plus two pixels, at most 512
evaluation pixels); distances are capped at the reach plus one. A texel is inside when its coverage
is at least one half; the signed distance is negative inside and measured to the boundary half a
texel away.

### Field Sample

`runtime::CompiledValueFieldSample` holds the program and its Scalar and Vector 2 operands as value
operands; the value evaluator samples curves at the frame and evaluates the program at the
position. A sampled field reads no image (an Image Field or a mask Shape Distance behind a Field
Sample reads transparent).

## Nodes

Category **Fields**. Every Scalar and Vector 2 parameter is animatable and has a socket of its own;
every choice, integer, boolean and seed is a constant that chooses the arithmetic. A Curve parameter
is a Curves master point list (at most 16 points) and a Ramp a Gradient value, both animatable as a
whole value. Field Remap's curve starts as the identity; Force Field's falloff curve starts falling
from full strength at the centre to none at the radius.

| Node | Outputs | Algorithm |
| --- | --- | --- |
| Noise | scalar, vector (two decorrelated streams in one instruction), gradient, curl, image, mask | below |
| Voronoi | distance, cell id, cell position, direction, edge distance, image, mask | Worley, "A Cellular Texture Basis Function", SIGGRAPH 1996: F1, F2, F2 − F1 over the 3 × 3 neighbourhood (5 × 5 for F2 with jitter above 1); exact edge distance by the two-pass method of Quilez's "Voronoi edges"; Smooth F1 by the exponential smooth minimum; Euclidean, Manhattan and Chebyshev metrics; evolution orbits each feature point and repeats at integers |
| Random Field | scalar in [0, 1), vector, colour, image, mask | integer hash of the cell (PCG-family mixing, as `bloom.random`); evolution steps to a new pattern each integer, Smooth crossfades; Gaussian by Box–Muller over two hashes |
| Pattern | scalar, image, mask | Stripes, Checker, Rings, Dots, Grid Lines, Hex, Wave with Sine, Triangle, Square and Saw profiles; edges filtered analytically over the softness (Quilez, "Filterable procedurals"); speed in cycles a second from exact clock time |
| Gradient Field | t, image, mask | Linear, Radial, Angular, Diamond between start and end, as the gradient generators; Clamp, Repeat, Mirror; cycle offset |
| Shape Distance | signed distance, falloff, direction, image, mask | Quilez's 2D signed distance functions (circle, box, rounded box, segment, polygon and star by angular folding, ring); a Mask source reads the exact distance image above; falloff over a distance with Linear, Smooth, Exponential or Step |
| Image Field | scalar, vector, colour | an image read at the field position (or at a `position` field, an absolute lookup); Bilinear, Nearest, Bicubic (Catmull–Rom); Clamp, Transparent, Repeat, Mirror edges; channel or channel pair |
| Force Field | vector, magnitude | Vortex, Radial, Uniform; strength, radius, spin; falloff Linear, Smooth, Inverse Square or a curve |
| Field Math | scalar, vector, colour | Add, Subtract, Multiply, Divide (zero where the divisor is zero), Min, Max, Power, Abs, Difference, Lerp, Screen, Smooth Min and Smooth Max (polynomial smooth minimum, k = smoothness), Dot, Length, Normalize, Scale, Rotate; an unconnected operand is its value parameter |
| Field Remap | scalar, vector | input range to output range, clamp, invert, steps, Schlick's bias and gain (Graphics Gems IV, 1994), a curve; a vector keeps its direction and remaps its length |
| Field Transform | scalar, vector, colour | translate, rotate, scale about a pivot, domain warp by a vector field times an amount (Quilez, "Domain warping"); Polar, Tile, Mirror and Kaleidoscope coordinates |
| Field Derivative | vector, scalar | Gradient, Curl, Divergence by central differences over the step; the analytic gradient when the input is a Noise scalar; the divergence of a scalar is zero |
| Combine Field / Separate Field | vector, colour / x, y, z, w | lanes, an unconnected lane its value |
| Field Sample | scalar, vector, colour values | the program at a position |
| Rasterize Field | image, mask | the raster above; Inspect draws a Heatmap through the ramp, Arrows on a grid, or Streamlines |

### Noise

**Vector output.** The vector output is the noise on stream 0 in x and the noise on stream 1 in y. It is
one Noise instruction (choice 8, the stream, is 2) that holds both: its block is followed by a second
block of the same size and stream 1, flagged by the header's `h3.y`, and its value is `(noise 0,
noise 1, 0, 0)`. Two instructions would each name the node's parameters, and the evaluator refuses a
plan in which two instructions share one parameter identity ("Compiled parameter identity is not
canonical"); one instruction names each once, for keyed values and for the scalar output alike. The scalar
output and its program are unchanged. (#647)

Types: **Perlin** (Perlin, "Improving Noise", SIGGRAPH 2002: quintic fade, twelve gradients; 3D
for evolution), **Simplex** (Gustavson, "Simplex noise demystified", 2005, in skewed lattice space),
and value noise with four interpolations: **Linear**, **Soft Linear** (smoothstep), **Block**
(nearest lattice value) and **Spline** (Catmull–Rom cubic). Flow evolution rotates the gradients
by the evolution phase (Perlin and Neyret, "Flow Noise", 2001; Gustavson and McEwan, psrdnoise,
JCGT 11(1), 2022); Slice evolution moves through the third lattice axis. Looping makes the
evolution periodic: with loop on, the evolution counts whole cycles over the loop length (zero:
the composition's duration), so frame 0 and the loop length match exactly.

Fractal sums (Musgrave, in Ebert et al., *Texturing and Modeling*, 3rd ed., 2003): each octave i
scales the frequency by the lacunarity, the amplitude by the gain, rotates by i × octave rotation,
shifts by the octave offset and by a fixed 37-cell decorrelation, and the last octave's fraction
weights it (fractional octaves animate smoothly). The sum is normalised by the sum of the weights.

| Bloom fractal | Layer-based compositor equivalent | Definition |
| --- | --- | --- |
| None | (one octave) | n |
| fBm | Basic | Σ wᵢ nᵢ |
| Turbulence | Turbulent Basic | Σ wᵢ abs(nᵢ) |
| Ridged | Turbulent Sharp | Musgrave's ridged multifractal: (ridge offset − abs(nᵢ))² weighted by the previous octave's signal times the ridge gain, clamped to [0, 1] |
| Billow | Turbulent Smooth | Σ wᵢ (2 abs(nᵢ) − 1) |
| Dynamic | Dynamic | fBm whose octave i evolves at phase × (i + 1) |
| Dynamic Progressive | Dynamic Progressive | octave i evolves at phase × 2ⁱ |
| Dynamic Twist | Dynamic Twist | octave i also rotates by i × phase turns |

The control names of that compositor map as: Complexity = octaves, Sub Influence = gain, Sub
Scaling = lacunarity, Sub Rotation = octave rotation, Sub Offset = octave offset, Offset
Turbulence = offset, Evolution = evolution, Cycle Evolution = loop; Invert flips the result.
Contrast and brightness apply around one half; **Overflow** then decides values outside [0, 1]:
Clip clamps, Soft Clamp is the identity on [1/4, 3/4] with an exponential knee towards 0 and 1
(continuous with its slope), Wrap Back folds back into range, and Allow HDR Results
(the default) keeps them. Range Bipolar maps to [−1, 1]. The gradient output is the analytic
derivative of the fractal sum; curl is the gradient turned a quarter turn (Bridson, Hourihan and
Nordenstam, "Curl-Noise for Procedural Fluid Flow", SIGGRAPH 2007), divergence-free.

## Evidence

- `src/core/tests/field_program_tests.cpp`: every operation's ranges, gradients, seeds, loops, far
  offsets, overflow, exact Voronoi at zero jitter, the polynomial elementary functions, the exact
  distance transform against brute force.
- `src/runtime/tests/field_raster_tests.cpp`: every operation rasters; a raster pixel is the Field
  Sample value at its centre; an Image slot returns stored texels exactly; sub-samples; the Mask
  mode; cache identity; loop periodicity; budget; cancellation; the mask distance against brute
  force.
- `src/runtime/tests/snapshot_compiler_field_tests.ipp`: lowering of rasters, one program for a
  chain, Explicit extents, Field Sample drivers, image slots, the four-image limit, mute.
- GPU coverage (`gpu_coverage_contract.hpp`): `effect.FieldRasterKernel`, `effect.MaskDistanceKernel`,
  `feature.field.op.<operation>` for every operation, `feature.field.images`,
  `feature.field.supersample`, `feature.field.output_modes` and a `node.<type>` fixture per Fields
  node are Required, with no exception.
- `src/runtime/tests/gpu_scene_effect_native_tests.cpp`: every operation (an Image field through
  the atlas), sub-samples and the mask distance on a device against the CPU oracle, one resident
  dispatch per pass and no readback, warm reuse with identical pixels and zero dispatches, a
  changed operand re-dispatching. Parity: at most 2% of pixels may differ, each otherwise within
  1e-4 relative (set when a device's `sqrt` could still round differently at cell and step
  boundaries; the coverage contract's native acceptance above is the strict gate).
- `tools/bench/scenario_spec.py`: the heavy-node scenario of a noise-driven Glow on synthetic media
  for cold and warm RAM preview.
