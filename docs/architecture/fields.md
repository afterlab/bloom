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
coordinate. The only operation a device may round differently is `sqrt`; the native parity gate
allows a bounded fraction of boundary pixels for that reason (below).

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
| Noise | scalar, vector (two decorrelated streams), gradient, curl, image, mask | below |
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
  1e-4 relative, for the `sqrt` rounding above at cell and step boundaries.
- `tools/bench/scenario_spec.py`: the heavy-node scenario of a noise-driven Glow on synthetic media
  for cold and warm RAM preview.
